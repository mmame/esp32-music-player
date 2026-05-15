/**
 * @file uart_master.cpp
 * @brief UART master protocol implementation for the player (Host) side.
 *
 * Runs a non-blocking state-machine parser on Core 0.
 * Transmit helpers are safe to call from any core / task.
 */

#include "uart_master.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "uart_master";

/* Typed UART port constant – avoids int→uart_port_t conversion errors in C++ */
static constexpr uart_port_t UART_PORT = static_cast<uart_port_t>(UM_UART_NUM);

/* ── Internal state ────────────────────────────────────────────────────────── */

static um_on_play_song_cb_t      s_on_play_song     = nullptr;
static um_on_stop_song_cb_t      s_on_stop_song     = nullptr;
static um_on_pause_cb_t          s_on_pause         = nullptr;
static um_on_resume_cb_t         s_on_resume        = nullptr;
static um_on_display_ready_cb_t  s_on_display_ready = nullptr;
static um_on_seek_cb_t           s_on_seek          = nullptr;

/** Semaphore posted by the rx task when CMD_ACK arrives (for uart_master_sync). */
static SemaphoreHandle_t s_ack_sem  = nullptr;

/** Mutex serialising concurrent calls to uart_write_bytes. */
static SemaphoreHandle_t s_tx_mutex = nullptr;

/* ── Parser state machine ──────────────────────────────────────────────────── */

typedef enum {
    PS_MAGIC = 0,
    PS_CMD,
    PS_LEN,
    PS_PAYLOAD,
    PS_CHECKSUM,
} parser_state_t;

/* ── Forward declarations ──────────────────────────────────────────────────── */
static void rx_task(void *arg);
static void handle_packet(uint8_t cmd, const uint8_t *payload, uint8_t len);

/* ── Generic framing helper ────────────────────────────────────────────────── */

static void send_packet(uint8_t cmd, const uint8_t *payload, uint8_t payload_len)
{
    if (payload_len > UM_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "send_packet: payload too large (%u), clamping", payload_len);
        payload_len = UM_MAX_PAYLOAD;
    }

    /* Compute checksum: CMD ^ LEN ^ payload bytes */
    uint8_t checksum = cmd ^ payload_len;
    for (uint8_t i = 0; i < payload_len; i++) {
        checksum ^= payload[i];
    }

    /* Frame: [MAGIC 8B][CMD 1B][LEN 1B][PAYLOAD][CHECKSUM 1B] */
    uint8_t buf[sizeof(UM_MAGIC) + 2 + UM_MAX_PAYLOAD + 1];
    memcpy(&buf[0], UM_MAGIC, sizeof(UM_MAGIC));
    buf[8] = cmd;
    buf[9] = payload_len;
    if (payload_len > 0) {
        memcpy(&buf[10], payload, payload_len);
    }
    buf[10 + payload_len] = checksum;

    const uint8_t total = (uint8_t)(sizeof(UM_MAGIC) + 2u + payload_len + 1u);

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT, buf, total);
    xSemaphoreGive(s_tx_mutex);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════════ */

void uart_master_init(um_on_play_song_cb_t     on_play_song,
                      um_on_stop_song_cb_t     on_stop_song,
                      um_on_pause_cb_t         on_pause,
                      um_on_resume_cb_t        on_resume,
                      um_on_display_ready_cb_t on_display_ready)
{
    s_on_play_song     = on_play_song;
    s_on_stop_song     = on_stop_song;
    s_on_pause         = on_pause;
    s_on_resume        = on_resume;
    s_on_display_ready = on_display_ready;

    s_ack_sem  = xSemaphoreCreateBinary();
    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_ack_sem  != nullptr);
    configASSERT(s_tx_mutex != nullptr);

    const uart_config_t cfg = {
        .baud_rate           = UM_BAUD_RATE,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UM_RX_BUF_SIZE, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                 UM_TX_PIN, UM_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d ready at %d baud (TX=%d, RX=%d)",
             UM_UART_NUM, UM_BAUD_RATE, UM_TX_PIN, UM_RX_PIN);

    BaseType_t ret = xTaskCreatePinnedToCore(
        rx_task, "uart_rx",
        4096, nullptr,
        configMAX_PRIORITIES - 2,
        nullptr,
        0 /* Core 0 */
    );
    configASSERT(ret == pdPASS);
}

void uart_master_set_seek_callback(um_on_seek_cb_t on_seek)
{
    s_on_seek = on_seek;
}

/* ── CMD_SONG_LIST ─────────────────────────────────────────────────────────── */

void uart_master_send_song_list(const char names[][UM_MAX_SONG_NAME], uint8_t count)
{
    uint8_t buf[UM_MAX_PAYLOAD];
    uint8_t buf_pos = 0;

    for (uint8_t i = 0; i < count; i++) {
        uint16_t    song_id  = (uint16_t)(i + 1);   /* 1-based */
        const char *name     = names[i];
        uint8_t     name_len = (uint8_t)strnlen(name, UM_MAX_SONG_NAME - 1);
        uint8_t     entry_sz = 2u + name_len + 1u;  /* id(2) + name + '\0' */

        if (buf_pos + entry_sz > UM_MAX_PAYLOAD) {
            send_packet(CMD_SONG_LIST, buf, buf_pos);
            buf_pos = 0;
        }

        buf[buf_pos++] = (uint8_t)(song_id & 0xFF);
        buf[buf_pos++] = (uint8_t)(song_id >> 8);
        memcpy(&buf[buf_pos], name, name_len);
        buf_pos += name_len;
        buf[buf_pos++] = '\0';
    }

    /* Append end-of-list terminator [0x00][0x00] */
    if (buf_pos + 2 > UM_MAX_PAYLOAD) {
        send_packet(CMD_SONG_LIST, buf, buf_pos);
        buf_pos = 0;
    }
    buf[buf_pos++] = 0x00;
    buf[buf_pos++] = 0x00;

    send_packet(CMD_SONG_LIST, buf, buf_pos);
    ESP_LOGI(TAG, "Sent song list (%u tracks)", count);
}

/* ── CMD_SET_STATE ─────────────────────────────────────────────────────────── */

void uart_master_send_state(const char *song_name,
                            uint8_t     is_playing,
                            uint8_t     volume,
                            uint8_t     tempo,
                            uint8_t     position_pct,
                            uint16_t    duration_s)
{
    uint8_t buf[UM_MAX_PAYLOAD];
    buf[0] = is_playing;
    buf[1] = volume;
    buf[2] = tempo;
    buf[3] = position_pct;
    buf[4] = (uint8_t)(duration_s & 0xFF);
    buf[5] = (uint8_t)(duration_s >> 8);

    uint8_t name_len = (uint8_t)strnlen(song_name, UM_MAX_PAYLOAD - 6 - 1);
    memcpy(&buf[6], song_name, name_len);
    uint8_t total = 6u + name_len;

    send_packet(CMD_SET_STATE, buf, total);
}

/* ── CMD_POTI_UPDATE ───────────────────────────────────────────────────────── */

void uart_master_send_poti_update(uint8_t volume, uint8_t tempo, uint8_t expression,
                                  uint8_t speed_min_x10, uint8_t speed_max_x10)
{
    uint8_t payload[5] = { volume, tempo, expression, speed_min_x10, speed_max_x10 };
    ESP_LOGD(TAG, "TX CMD_POTI_UPDATE vol=%u tempo=%u expr=%u spd_min=%u spd_max=%u",
             volume, tempo, expression, speed_min_x10, speed_max_x10);
    send_packet(CMD_POTI_UPDATE, payload, 5);
}

/* ── CMD_ENCODER_MOVE ──────────────────────────────────────────────────────── */

void uart_master_send_encoder_move(int8_t delta)
{
    uint8_t payload[1] = { (uint8_t)delta };
    ESP_LOGD(TAG, "TX CMD_ENCODER_MOVE delta=%+d", delta);
    send_packet(CMD_ENCODER_MOVE, payload, 1);
}

/* ── CMD_ENCODER_BTN ───────────────────────────────────────────────────────── */

void uart_master_send_encoder_btn(void)
{
    ESP_LOGD(TAG, "TX CMD_ENCODER_BTN");
    send_packet(CMD_ENCODER_BTN, nullptr, 0);
}

/* ── CMD_SYNC ──────────────────────────────────────────────────────────────── */

bool uart_master_sync(uint32_t timeout_ms)
{
    /* Drain any stale ACK token */
    xSemaphoreTake(s_ack_sem, 0);

    ESP_LOGI(TAG, "TX CMD_SYNC (timeout %u ms)", (unsigned)timeout_ms);
    send_packet(CMD_SYNC, nullptr, 0);

    bool got_ack = (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
    if (got_ack) {
        ESP_LOGI(TAG, "SYNC: ACK received");
    } else {
        ESP_LOGW(TAG, "SYNC: no ACK within %u ms", (unsigned)timeout_ms);
    }
    return got_ack;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Receive task & parser (Core 0)
 * ══════════════════════════════════════════════════════════════════════════════ */

static void rx_task(void *arg)
{
    parser_state_t state       = PS_MAGIC;
    uint8_t        magic_idx   = 0;
    uint8_t        cmd         = 0;
    uint8_t        payload_len = 0;
    uint8_t        payload[UM_MAX_PAYLOAD];
    uint8_t        payload_idx = 0;
    uint8_t        checksum    = 0;
    uint8_t        byte;

    ESP_LOGI(TAG, "RX task running on core %d", xPortGetCoreID());

    while (true) {
        if (uart_read_bytes(UART_PORT, &byte, 1, pdMS_TO_TICKS(50)) != 1) {
            continue;
        }

        switch (state) {

        case PS_MAGIC:
            if (byte == UM_MAGIC[magic_idx]) {
                magic_idx++;
                if (magic_idx == sizeof(UM_MAGIC)) {
                    magic_idx = 0;
                    state = PS_CMD;
                }
            } else {
                magic_idx = (byte == UM_MAGIC[0]) ? 1 : 0;
            }
            break;

        case PS_CMD:
            cmd      = byte;
            checksum = byte;
            state    = PS_LEN;
            break;

        case PS_LEN:
            payload_len  = byte;
            checksum    ^= byte;
            payload_idx  = 0;
            if (payload_len == 0) {
                state = PS_CHECKSUM;
            } else if (payload_len <= UM_MAX_PAYLOAD) {
                state = PS_PAYLOAD;
            } else {
                ESP_LOGW(TAG, "Payload length %u > %u, discarding", payload_len, UM_MAX_PAYLOAD);
                state = PS_MAGIC;
            }
            break;

        case PS_PAYLOAD:
            payload[payload_idx++] = byte;
            checksum ^= byte;
            if (payload_idx >= payload_len) {
                state = PS_CHECKSUM;
            }
            break;

        case PS_CHECKSUM:
            if (byte == checksum) {
                handle_packet(cmd, payload, payload_len);
            } else {
                ESP_LOGW(TAG, "Checksum fail: expected 0x%02X got 0x%02X", checksum, byte);
            }
            state = PS_MAGIC;
            break;

        default:
            state = PS_MAGIC;
            break;
        }
    }
}

/* ── Packet dispatcher ─────────────────────────────────────────────────────── */

static void handle_packet(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch (cmd) {

    case CMD_PLAY_SONG:
        if (len < 2) {
            ESP_LOGW(TAG, "CMD_PLAY_SONG: payload too short");
            break;
        }
        {
            uint16_t song_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            ESP_LOGI(TAG, "CMD_PLAY_SONG: id=%u", song_id);
            if (s_on_play_song) s_on_play_song(song_id);
        }
        break;

    case CMD_STOP_SONG:
        ESP_LOGI(TAG, "CMD_STOP_SONG received");
        if (s_on_stop_song) s_on_stop_song();
        break;

    case CMD_PAUSE:
        ESP_LOGI(TAG, "CMD_PAUSE received");
        if (s_on_pause) s_on_pause();
        break;

    case CMD_RESUME:
        ESP_LOGI(TAG, "CMD_RESUME received");
        if (s_on_resume) s_on_resume();
        break;

    case CMD_DISPLAY_READY:
        ESP_LOGI(TAG, "CMD_DISPLAY_READY received – display was reset");
        if (s_on_display_ready) s_on_display_ready();
        break;

    case CMD_SEEK:
        if (len < 1) {
            ESP_LOGW(TAG, "CMD_SEEK: missing payload");
            break;
        }
        {
            uint8_t pct = payload[0];
            if (pct > 100) pct = 100;
            ESP_LOGI(TAG, "CMD_SEEK: %u%%", pct);
            if (s_on_seek) s_on_seek(pct);
        }
        break;

    case CMD_ACK:
        ESP_LOGD(TAG, "CMD_ACK received");
        xSemaphoreGive(s_ack_sem);
        break;

    default:
        ESP_LOGW(TAG, "Unknown CMD 0x%02X (len=%u)", cmd, len);
        break;
    }
}
