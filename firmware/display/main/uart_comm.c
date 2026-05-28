/**
 * @file uart_comm.c
 * @brief UART communication layer – state-machine parser + FreeRTOS task.
 *
 * Packet format (all multi-byte fields are little-endian):
 *   [MAGIC 8B][CMD 1B][LEN 1B][PAYLOAD LEN*B][CHECKSUM 1B]
 *
 * Checksum = XOR of (CMD ^ LEN ^ payload[0] ^ ... ^ payload[LEN-1])
 *
 * CMD_SET_STATE (0x01) payload layout:
 *   [is_playing:u8][volume:u8][tempo:u8][song_name:char[0..MAX_SONG_NAME_LEN-1]]
 *
 * CMD_SYNC (0x02) has no payload (LEN = 0).  Both CMD_SYNC and CMD_SET_STATE
 * elicit a CMD_ACK (0xFF) response from the display.  Extended ACK payload:
 *   [touch_active:u8][touch_x:i16_le][touch_y:i16_le][cmd_count:u8]
 *   Followed by cmd_count sub-commands, each: [cmd_id:u8][param_len:u8][params:...]
 *
 * The display NEVER sends unsolicited packets.  All Display->Host commands
 * are queued via enqueue_pending_cmd() and flushed in the next ACK response.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

#include "uart_comm.h"
#include "ui_songlist.h"
#include "ui_player.h"

static const char *TAG = "uart_comm";

/* ---------- Global state ------------------------------------------------- */
music_player_state_t g_player_state = {
    .song_name  = "",
    .is_playing = 0,
    .volume     = 0,
    .tempo      = 0,
};

/** Mutex protecting g_player_state for cross-task access. */
static SemaphoreHandle_t s_state_mutex = NULL;

/** Track previous is_playing to detect play/stop transitions. */
static uint8_t s_was_playing = 0;

/* ---------- CMD_SONG_LIST accumulation ----------------------------------- */

/**
 * Multiple CMD_SONG_LIST packets may arrive before the end-of-list terminator.
 * We accumulate all chunks here and only forward the merged buffer to the UI
 * once the terminator (song_id == 0, name == '\0') is detected.
 */
static uint8_t  s_songlist_buf[SONGLIST_BUF_SIZE];
static uint16_t s_songlist_len = 0;

/* ---------- Touch state -------------------------------------------------- */

/**
 * Touch state is updated from the LVGL indev callback (LVGL task, Core 1)
 * and read from the UART task (Core 0).  A spinlock is used so the
 * touch-state update path never blocks (important for callbacks).
 */
static portMUX_TYPE  s_touch_mux    = portMUX_INITIALIZER_UNLOCKED;
static volatile bool    s_touch_active = false;
static volatile int16_t s_touch_x      = 0;
static volatile int16_t s_touch_y      = 0;

/* ---------- Pending command queue ---------------------------------------- */

/**
 * All Display->Host commands are queued here instead of being sent
 * immediately.  They are flushed in send_response(), which is called
 * whenever CMD_SET_STATE or CMD_SYNC is received from the player.
 *
 * Access to the queue is protected by a spinlock so it can be safely
 * written from the LVGL task (Core 1) and read from the UART task (Core 0).
 */
#define PENDING_QUEUE_SLOTS    8   /* max simultaneous queued commands  */
#define PENDING_CMD_MAX_PARAMS 4   /* max param bytes per command        */

typedef struct {
    uint8_t cmd_id;
    uint8_t param_len;
    uint8_t params[PENDING_CMD_MAX_PARAMS];
} pending_cmd_t;

static pending_cmd_t s_pending_queue[PENDING_QUEUE_SLOTS];
static uint8_t       s_pending_count = 0;
static portMUX_TYPE  s_queue_mux     = portMUX_INITIALIZER_UNLOCKED;

/* ---------- Parser state machine ----------------------------------------- */

typedef enum {
    PARSER_STATE_MAGIC,     /* Matching magic header byte-by-byte */
    PARSER_STATE_CMD,       /* Reading command byte                */
    PARSER_STATE_LEN,       /* Reading payload length byte         */
    PARSER_STATE_PAYLOAD,   /* Accumulating payload bytes          */
    PARSER_STATE_CHECKSUM,  /* Reading and verifying checksum      */
} parser_state_t;

/* ---------- Forward declarations ----------------------------------------- */
static void uart_task(void *arg);
static void handle_packet(uint8_t cmd, const uint8_t *payload, uint8_t len);
static void enqueue_pending_cmd(uint8_t cmd_id, const uint8_t *params, uint8_t param_len);
static void send_response(void);

/* =========================================================================
 * Public API
 * ========================================================================= */

void uart_comm_send_play_song(uint16_t song_id)
{
    uint8_t params[2];
    params[0] = (uint8_t)(song_id & 0xFF);
    params[1] = (uint8_t)(song_id >> 8);
    enqueue_pending_cmd(CMD_PLAY_SONG, params, 2);
    ESP_LOGI(TAG, "CMD_PLAY_SONG queued: song_id=%u", song_id);
}

void uart_comm_send_stop(void)
{
    enqueue_pending_cmd(CMD_STOP_SONG, NULL, 0);
    ESP_LOGI(TAG, "CMD_STOP_SONG queued");
}

void uart_comm_send_pause(void)
{
    enqueue_pending_cmd(CMD_PAUSE, NULL, 0);
    ESP_LOGI(TAG, "CMD_PAUSE queued");
}

void uart_comm_send_resume(void)
{
    enqueue_pending_cmd(CMD_RESUME, NULL, 0);
    ESP_LOGI(TAG, "CMD_RESUME queued");
}

void uart_comm_send_seek(uint8_t pct)
{
    enqueue_pending_cmd(CMD_SEEK, &pct, 1);
    ESP_LOGI(TAG, "CMD_SEEK queued: pct=%u", pct);
}

void uart_comm_send_st_bypass(bool bypass)
{
    uint8_t val = bypass ? 0x01u : 0x00u;
    enqueue_pending_cmd(CMD_ST_BYPASS, &val, 1);
    ESP_LOGI(TAG, "CMD_ST_BYPASS queued: %s", bypass ? "ON" : "OFF");
}

void uart_comm_send_tempo_lock(bool lock, uint8_t locked_tempo)
{
    uint8_t params[2];
    params[0] = lock ? 0x01u : 0x00u;
    params[1] = locked_tempo;
    enqueue_pending_cmd(CMD_TEMPO_LOCK, params, 2);
    ESP_LOGI(TAG, "CMD_TEMPO_LOCK queued: %s tempo=%u",
             lock ? "LOCK" : "UNLOCK", (unsigned)locked_tempo);
}

void uart_comm_send_wifi_ctrl(bool enable)
{
    uint8_t val = enable ? 0x01u : 0x00u;
    enqueue_pending_cmd(CMD_WIFI_CTRL, &val, 1);
    ESP_LOGI(TAG, "CMD_WIFI_CTRL queued: %s", enable ? "ENABLE" : "DISABLE");
}

void uart_comm_send_song_settings_req(uint16_t song_id)
{
    uint8_t params[2];
    params[0] = (uint8_t)(song_id & 0xFF);
    params[1] = (uint8_t)(song_id >> 8);
    enqueue_pending_cmd(CMD_SONG_SETTINGS_REQ, params, 2);
    ESP_LOGI(TAG, "CMD_SONG_SETTINGS_REQ queued: song_id=%u", song_id);
}

void uart_comm_send_set_song_settings(uint16_t song_id,
                                      uint8_t  flags,
                                      uint8_t  fixed_speed_x100)
{
    uint8_t params[4];
    params[0] = (uint8_t)(song_id & 0xFF);
    params[1] = (uint8_t)(song_id >> 8);
    params[2] = flags;
    params[3] = fixed_speed_x100;
    enqueue_pending_cmd(CMD_SET_SONG_SETTINGS, params, 4);
    ESP_LOGI(TAG, "CMD_SET_SONG_SETTINGS queued: id=%u flags=0x%02X spd=%u",
             song_id, flags, fixed_speed_x100);
}

void uart_comm_init(void)
{
    /* Create state mutex before the task can use it */
    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex != NULL);

    const uart_config_t uart_cfg = {
        .baud_rate  = UART_COMM_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_COMM_PORT,
                                        UART_COMM_RX_BUF_SIZE, 0,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_COMM_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_COMM_PORT,
                                 UART_COMM_TX_PIN, UART_COMM_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d initialised at %d baud (TX=%d, RX=%d)",
             UART_COMM_PORT, UART_COMM_BAUD_RATE,
             UART_COMM_TX_PIN, UART_COMM_RX_PIN);

    /* Pin UART task to Core 0 (protocol/communication core) */
    BaseType_t ret = xTaskCreatePinnedToCore(
        uart_task, "uart_comm",
        4096, NULL,
        tskIDLE_PRIORITY + 2,
        NULL, 0);
    configASSERT(ret == pdPASS);

    /* Queue CMD_DISPLAY_READY so the player knows the display has (re)started
     * and should resend the song list.  It will be delivered in the first
     * CMD_ACK response (to CMD_SYNC or CMD_SET_STATE). */
    enqueue_pending_cmd(CMD_DISPLAY_READY, NULL, 0);
    ESP_LOGI(TAG, "CMD_DISPLAY_READY queued");
}

void uart_comm_update_touch(bool active, int16_t x, int16_t y)
{
    taskENTER_CRITICAL(&s_touch_mux);
    s_touch_active = active;
    if (active) {
        s_touch_x = x;
        s_touch_y = y;
    }
    taskEXIT_CRITICAL(&s_touch_mux);
}

/* =========================================================================
 * UART task – runs on Core 0
 * ========================================================================= */

static void uart_task(void *arg)
{
    parser_state_t state      = PARSER_STATE_MAGIC;
    uint8_t        magic_idx  = 0;
    uint8_t        cmd        = 0;
    uint8_t        len        = 0;
    uint8_t        payload[MAX_PAYLOAD_LEN];
    uint8_t        payload_idx = 0;
    uint8_t        checksum   = 0;
    uint8_t        byte;

    ESP_LOGI(TAG, "UART task started on core %d", xPortGetCoreID());

    while (1) {
        /* Block up to 100 ms waiting for a byte */
        if (uart_read_bytes(UART_COMM_PORT, &byte, 1, pdMS_TO_TICKS(100)) != 1) {
            continue; /* Timeout – nothing received, keep polling */
        }

        switch (state) {

        /* ---- Magic header matching ------------------------------------ */
        case PARSER_STATE_MAGIC:
            if (byte == UART_MAGIC_BYTES[magic_idx]) {
                magic_idx++;
                if (magic_idx == sizeof(UART_MAGIC_BYTES)) {
                    magic_idx = 0;
                    state = PARSER_STATE_CMD;
                    ESP_LOGD(TAG, "Magic header matched");
                }
            } else {
                /*
                 * Partial-match recovery: if the mismatched byte equals the
                 * first magic byte, count it as the new start of the header.
                 */
                magic_idx = (byte == UART_MAGIC_BYTES[0]) ? 1 : 0;
            }
            break;

        /* ---- Command byte -------------------------------------------- */
        case PARSER_STATE_CMD:
            cmd      = byte;
            checksum = byte; /* Checksum accumulator starts with CMD */
            state    = PARSER_STATE_LEN;
            break;

        /* ---- Payload length ------------------------------------------ */
        case PARSER_STATE_LEN:
            len       = byte;
            checksum ^= byte;
            payload_idx = 0;

            if (len == 0) {
                state = PARSER_STATE_CHECKSUM;
            } else if (len <= MAX_PAYLOAD_LEN) {
                state = PARSER_STATE_PAYLOAD;
            } else {
                ESP_LOGW(TAG, "Payload length %u exceeds MAX_PAYLOAD_LEN, discarding packet", len);
                state = PARSER_STATE_MAGIC;
            }
            break;

        /* ---- Payload bytes ------------------------------------------- */
        case PARSER_STATE_PAYLOAD:
            payload[payload_idx++] = byte;
            checksum ^= byte;
            if (payload_idx >= len) {
                state = PARSER_STATE_CHECKSUM;
            }
            break;

        /* ---- Checksum verification ------------------------------------ */
        case PARSER_STATE_CHECKSUM:
            if (byte == checksum) {
                handle_packet(cmd, payload, len);
            } else {
                ESP_LOGW(TAG, "Checksum mismatch: expected 0x%02X, got 0x%02X – packet discarded",
                         checksum, byte);
            }
            state = PARSER_STATE_MAGIC; /* Always reset for next packet */
            break;

        default:
            state = PARSER_STATE_MAGIC;
            break;
        }
    }
}

/* =========================================================================
 * Packet dispatcher
 * ========================================================================= */

static void handle_packet(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch (cmd) {

    /* ------------------------------------------------------------------ */
    case CMD_SET_STATE: {
        /*
         * Payload layout (new, 6-byte header):
         *   [0]       is_playing   : uint8_t
         *   [1]       volume       : uint8_t  (0–100)
         *   [2]       tempo        : uint8_t  (0–100, 50 = 1.0×)
         *   [3]       position_pct : uint8_t  (0–100 %)
         *   [4..5]    duration_s   : uint16_t (speed-adjusted length, LE)
         *   [6..N-1]  song_name    : char[]   (not null-terminated in packet)
         *
         * Old layout (3-byte header) is still accepted with a warning.
         */
        if (len < 3) {
            ESP_LOGW(TAG, "CMD_SET_STATE: payload too short (%u bytes)", len);
            break;
        }

        uint8_t new_is_playing = payload[0];

        uint8_t  position_pct = 0;
        uint16_t duration_s   = 0;
        uint8_t  name_offset  = 3;

        if (len >= 6) {
            position_pct = payload[3];
            duration_s   = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
            name_offset  = 6;
        } else {
            ESP_LOGW(TAG, "CMD_SET_STATE: old 3-byte header (len=%u), position/duration unavailable", len);
        }

        /* Snapshot song name directly from payload before taking mutex */
        char song_name_snap[MAX_SONG_NAME_LEN] = {0};
        if (len > name_offset) {
            uint8_t name_len = len - name_offset;
            if (name_len >= MAX_SONG_NAME_LEN) name_len = MAX_SONG_NAME_LEN - 1;
            memcpy(song_name_snap, &payload[name_offset], name_len);
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        g_player_state.is_playing   = new_is_playing;
        g_player_state.volume       = payload[1];
        g_player_state.tempo        = payload[2];
        g_player_state.position_pct = position_pct;
        g_player_state.duration_s   = duration_s;
        memcpy(g_player_state.song_name, song_name_snap, MAX_SONG_NAME_LEN);
        xSemaphoreGive(s_state_mutex);

        ESP_LOGD(TAG, "State: song='%s'  vol=%u  tempo=%u  playing=%u  pos=%u%%  dur=%us",
                 song_name_snap, g_player_state.volume, g_player_state.tempo,
                 new_is_playing, position_pct, duration_s);

        /* Trigger view transitions on play/stop edges */
        if (new_is_playing && !s_was_playing) {
            ui_player_show_async(song_name_snap);
        } else if (!new_is_playing && s_was_playing) {
            ui_player_hide_async();
        }
        s_was_playing = new_is_playing;

        /* Forward live progress to the UI every tick while playing */
        if (new_is_playing) {
            ui_player_update_progress_async(position_pct, duration_s);
        }

        /* Send poll response – flushes any queued Display->Host commands */
        send_response();
        break;
    }

    /* ------------------------------------------------------------------ */
    case CMD_SYNC:
        ESP_LOGD(TAG, "CMD_SYNC received – sending response");
        send_response();
        break;

    /* ------------------------------------------------------------------ */
    case CMD_SONG_LIST: {
        /*
         * Payload: packed null-terminated song entries.
         * Each entry: [id_lo:u8][id_hi:u8][name:char...]['\0']
         * Terminator: song_id == 0 followed by '\0' name byte.
         *
         * The host may split the list across several CMD_SONG_LIST packets.
         * Accumulate chunks until the terminator is found, then hand the
         * merged buffer to the UI layer in one call.
         */
        ESP_LOGI(TAG, "CMD_SONG_LIST chunk received (%u bytes, accumulated=%u)",
                 len, s_songlist_len);

        /* Guard against buffer overflow – discard and restart on overflow */
        if ((uint32_t)s_songlist_len + len > SONGLIST_BUF_SIZE) {
            ESP_LOGW(TAG, "CMD_SONG_LIST: accumulation buffer overflow – discarding");
            s_songlist_len = 0;
        }
        if (len > 0) {
            memcpy(s_songlist_buf + s_songlist_len, payload, len);
            s_songlist_len += len;
        }

        /* Scan accumulated data for the end-of-list terminator:
         * entry with song_id == 0 immediately followed by a '\0' name byte. */
        bool found_terminator = false;
        const uint8_t *scan     = s_songlist_buf;
        const uint8_t *scan_end = s_songlist_buf + s_songlist_len;
        while (scan + 2 <= scan_end) {
            uint16_t sid = (uint16_t)scan[0] | ((uint16_t)scan[1] << 8);
            scan += 2;
            /* Accept terminator whether it carries an empty name byte or not */
            if (sid == 0 && (scan >= scan_end || *scan == '\0')) {
                found_terminator = true;
                break;
            }
            /* Skip past the null-terminated song name */
            while (scan < scan_end && *scan != '\0') scan++;
            if (scan < scan_end) scan++; /* consume '\0' */
        }

        if (found_terminator) {
            ESP_LOGI(TAG, "CMD_SONG_LIST complete: %u bytes total", s_songlist_len);
            ui_songlist_update_async(s_songlist_buf, s_songlist_len);
            s_songlist_len = 0; /* reset for next list transfer */
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case CMD_ENCODER_MOVE:
        /*
         * Payload: 1 signed byte – encoder delta (positive = CW / down).
         */
        if (len < 1) {
            ESP_LOGW(TAG, "CMD_ENCODER_MOVE: missing payload");
            break;
        }
        {
            int8_t delta = (int8_t)payload[0];
            ESP_LOGD(TAG, "CMD_ENCODER_MOVE delta=%d", delta);
            ui_songlist_encoder_move_async(delta);
        }
        break;

    /* ------------------------------------------------------------------ */
    case CMD_ENCODER_BTN:
        ESP_LOGD(TAG, "CMD_ENCODER_BTN received");
        ui_songlist_encoder_btn_async();
        break;

    /* ------------------------------------------------------------------ */
    case CMD_POTI_UPDATE:
        /*
         * Payload layout (5 bytes):
         *   [0]  volume        : uint8_t  (0–100)
         *   [1]  tempo         : uint8_t  (0–100, 50 = 1.0×)
         *   [2]  expression    : uint8_t  (0–100, reserved)
         *   [3]  speed_min_x10 : uint8_t  (SPEED_MIN × 10)
         *   [4]  speed_max_x10 : uint8_t  (SPEED_MAX × 10)
         */
        if (len < 3) {
            ESP_LOGW(TAG, "CMD_POTI_UPDATE: payload too short (%u bytes)", len);
            break;
        }
        {
            uint8_t speed_min_x10 = (len >= 5) ? payload[3] : 4;   /* default 0.4× */
            uint8_t speed_max_x10 = (len >= 5) ? payload[4] : 20;  /* default 2.0× */
            ui_player_update_potis_async(payload[0], payload[1], speed_min_x10, speed_max_x10);
        }
        break;

    /* ------------------------------------------------------------------ */
    case CMD_SONG_SETTINGS: {
        /*
         * Payload layout (4 bytes):
         *   [0..1]  song_id          : uint16_t  (LE)
         *   [2]     flags            : uint8_t   (bit 0 = loop, bit 1 = fixed_speed_en)
         *   [3]     fixed_speed_x100 : uint8_t   (speed × 100, e.g. 100 = 1.0×)
         */
        if (len < 4) {
            ESP_LOGW(TAG, "CMD_SONG_SETTINGS: payload too short (%u)", len);
            break;
        }
        uint16_t song_id          = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        uint8_t  flags            = payload[2];
        uint8_t  fixed_speed_x100 = payload[3];
        ESP_LOGI(TAG, "CMD_SONG_SETTINGS id=%u flags=0x%02X spd=%u",
                 song_id, flags, fixed_speed_x100);
        ui_songlist_song_settings_async(song_id, flags, fixed_speed_x100);
        ui_player_song_settings_async(song_id, flags, fixed_speed_x100);
        break;
    }

    /* ------------------------------------------------------------------ */
    default:
        ESP_LOGW(TAG, "Unknown command 0x%02X (len=%u) – ignored", cmd, len);
        break;
    }
}

/* =========================================================================
 * Pending command queue helper
 * ========================================================================= */

/**
 * Atomically push one command onto the pending queue.
 * Safe to call from any task or interrupt context.
 * If the queue is full the command is silently dropped.
 */
static void enqueue_pending_cmd(uint8_t cmd_id, const uint8_t *params, uint8_t param_len)
{
    if (param_len > PENDING_CMD_MAX_PARAMS) param_len = PENDING_CMD_MAX_PARAMS;
    bool dropped = false;
    taskENTER_CRITICAL(&s_queue_mux);
    if (s_pending_count < PENDING_QUEUE_SLOTS) {
        pending_cmd_t *slot = &s_pending_queue[s_pending_count++];
        slot->cmd_id    = cmd_id;
        slot->param_len = param_len;
        if (param_len > 0 && params != NULL) {
            memcpy(slot->params, params, param_len);
        }
    } else {
        dropped = true;
    }
    taskEXIT_CRITICAL(&s_queue_mux);
    if (dropped) {
        ESP_LOGW(TAG, "Pending queue full – cmd 0x%02X dropped", cmd_id);
    }
}

/* =========================================================================
 * Response sender (CMD_ACK with queued sub-commands)
 * ========================================================================= */

/**
 * Build and transmit a CMD_ACK (0xFF) packet carrying the current touch state
 * and any queued Display->Host commands.  Called from handle_packet() on
 * CMD_SET_STATE and CMD_SYNC receipt; never called from any other context.
 *
 * Extended payload layout:
 *   [0]     touch_active : uint8_t
 *   [1..2]  touch_x      : int16_t  LE
 *   [3..4]  touch_y      : int16_t  LE
 *   [5]     cmd_count    : uint8_t
 *   For each sub-command:
 *     [n]   cmd_id       : uint8_t
 *     [n+1] param_len    : uint8_t
 *     [n+2..n+1+param_len] params
 */
static void send_response(void)
{
    /* Snapshot touch state */
    bool    touch_active;
    int16_t touch_x, touch_y;
    taskENTER_CRITICAL(&s_touch_mux);
    touch_active = s_touch_active;
    touch_x      = s_touch_x;
    touch_y      = s_touch_y;
    taskEXIT_CRITICAL(&s_touch_mux);

    /* Atomically drain the pending command queue */
    uint8_t       count;
    pending_cmd_t cmds[PENDING_QUEUE_SLOTS];
    taskENTER_CRITICAL(&s_queue_mux);
    count           = s_pending_count;
    memcpy(cmds, s_pending_queue, count * sizeof(pending_cmd_t));
    s_pending_count = 0;
    taskEXIT_CRITICAL(&s_queue_mux);

    /* Build payload */
    uint8_t payload[MAX_PAYLOAD_LEN];
    uint8_t pos = 0;

    payload[pos++] = touch_active ? 1u : 0u;
    payload[pos++] = (uint8_t)( touch_x       & 0xFF);
    payload[pos++] = (uint8_t)((touch_x >> 8) & 0xFF);
    payload[pos++] = (uint8_t)( touch_y       & 0xFF);
    payload[pos++] = (uint8_t)((touch_y >> 8) & 0xFF);

    uint8_t count_idx = pos;   /* will be patched if truncation occurs */
    payload[pos++]    = count;

    uint8_t sent = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t needed = 2u + cmds[i].param_len;
        if (pos + needed > MAX_PAYLOAD_LEN) {
            ESP_LOGW(TAG, "Response payload full – %u/%u sub-commands sent", sent, count);
            payload[count_idx] = sent;
            break;
        }
        payload[pos++] = cmds[i].cmd_id;
        payload[pos++] = cmds[i].param_len;
        if (cmds[i].param_len > 0) {
            memcpy(&payload[pos], cmds[i].params, cmds[i].param_len);
            pos += cmds[i].param_len;
        }
        sent++;
    }
    payload[count_idx] = sent;

    /* Compute checksum: CMD ^ LEN ^ payload[0..pos-1] */
    uint8_t checksum = CMD_ACK ^ pos;
    for (uint8_t i = 0; i < pos; i++) checksum ^= payload[i];

    /* Assemble and transmit */
    uint8_t pkt[8 + 2 + MAX_PAYLOAD_LEN + 1];
    memcpy(&pkt[0], UART_MAGIC_BYTES, 8);
    pkt[8] = CMD_ACK;
    pkt[9] = pos;
    memcpy(&pkt[10], payload, pos);
    pkt[10 + pos] = checksum;

    uart_write_bytes(UART_COMM_PORT, pkt, (size_t)(11u + pos));

    ESP_LOGD(TAG, "Response sent: touch=%d x=%d y=%d cmds=%u",
             touch_active, touch_x, touch_y, sent);
}
