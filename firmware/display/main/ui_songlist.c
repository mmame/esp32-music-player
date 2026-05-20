/**
 * @file ui_songlist.c
 * @brief Full-screen LVGL songlist view with encoder navigation.
 *
 * Architecture
 * ------------
 * - The UART task (Core 0) calls the *_async() helpers which use
 *   lv_async_call() to post work items to the LVGL task (Core 1).
 * - The LVGL task executes the work items inside lv_timer_handler(),
 *   so no extra locking is needed there.
 * - cmd_play_song() runs in the LVGL task but only calls uart_write_bytes()
 *   which is ISR-safe and reentrant, so no extra mutex is required.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#include "lvgl.h"
#include "ui_songlist.h"
#include "uart_comm.h"

static const char *TAG = "ui_songlist";

/* ---------- Internal state ----------------------------------------------- */

typedef struct {
    uint16_t id;
    char     name[MAX_SONG_NAME_LEN];
} song_entry_t;

static song_entry_t s_songs[SONGLIST_MAX_SONGS];
static uint8_t      s_song_count = 0;
static int16_t      s_focused_idx = 0;  /* currently focused list-button index */

/* LVGL objects */
static lv_obj_t   *s_screen      = NULL;
static lv_obj_t   *s_list        = NULL;
static lv_group_t *s_group       = NULL;
static lv_indev_t *s_enc_indev   = NULL; /* virtual encoder indev */

/* WiFi toggle button */
static lv_obj_t *s_wifi_btn   = NULL;   /* clickable container */
static lv_obj_t *s_wifi_icon  = NULL;   /* label: LV_SYMBOL_WIFI */
static lv_obj_t *s_wifi_slash = NULL;   /* "\" overlay when disabled */
static bool      s_wifi_enabled = false; /* starts disabled */

/* ---------- Forward declarations ----------------------------------------- */
static void on_list_item_clicked(lv_event_t *e);
static void on_wifi_btn_clicked(lv_event_t *e);
static void update_wifi_btn_style(void);
static void send_play_song(uint16_t song_id);
static void focus_item(int16_t idx);
static void create_wifi_info_popup(void);
static void on_wifi_info_ok(lv_event_t *e);

/* ---------- async payload structs ---------------------------------------- */

typedef struct {
    uint8_t *data; /* heap-allocated; freed by the async callback */
    uint16_t len;
} async_songlist_payload_t;

typedef struct {
    int8_t steps;
} async_encoder_move_t;

/* =========================================================================
 * WiFi button helpers
 * ========================================================================= */

static void update_wifi_btn_style(void)
{
    if (s_wifi_enabled) {
        /* WiFi on: blue button, white icon, slash hidden */
        lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_bg_opa(s_wifi_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_white(), 0);
        lv_obj_add_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* WiFi off: dark grey button, greyed icon, slash visible */
        lv_obj_set_style_bg_color(s_wifi_btn, lv_color_hex(0x2A2A3E), 0);
        lv_obj_set_style_bg_opa(s_wifi_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(0x505060), 0);
        lv_obj_clear_flag(s_wifi_slash, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wifi_btn_clicked(lv_event_t *e)
{
    (void)e;
    s_wifi_enabled = !s_wifi_enabled;
    update_wifi_btn_style();
    uart_comm_send_wifi_ctrl(s_wifi_enabled);
    ESP_LOGI(TAG, "WiFi icon toggled: %s", s_wifi_enabled ? "ENABLE" : "DISABLE");
    if (s_wifi_enabled) {
        create_wifi_info_popup();
    }
}

/* =========================================================================
 * WiFi info popup
 * ========================================================================= */

static void on_wifi_info_ok(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_delete(overlay);
}

static void create_wifi_info_popup(void)
{
    /* Full-screen semi-transparent backdrop – blocks input to the list below */
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* Dialog box */
    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, 520, 330);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_pad_all(box, 22, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WiFi Enabled");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1E88E5), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* Instructions */
    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg,
        "1. Connect to WiFi network:\n"
        "        \"MusicPlayer\"\n"
        "\n"
        "2. Open in your browser:\n"
        "        192.168.4.1");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xE0E0FF), 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, 476);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 50);

    /* OK button */
    lv_obj_t *btn = lv_obj_create(box);
    lv_obj_set_size(btn, 150, 54);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, on_wifi_info_ok, LV_EVENT_CLICKED, overlay);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "OK");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_center(btn_lbl);
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

void ui_songlist_create(void)
{
    /* Full-screen dark background */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    /* Title label */
    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "SONG LIST");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* WiFi toggle button – top-right of the header strip */
    s_wifi_btn = lv_obj_create(s_screen);
    lv_obj_set_size(s_wifi_btn, 46, 44);
    lv_obj_align(s_wifi_btn, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_border_width(s_wifi_btn, 0, 0);
    lv_obj_set_style_radius(s_wifi_btn, 8, 0);
    lv_obj_set_style_pad_all(s_wifi_btn, 0, 0);
    lv_obj_clear_flag(s_wifi_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_wifi_btn, on_wifi_btn_clicked, LV_EVENT_CLICKED, NULL);

    /* WiFi icon label inside the button */
    s_wifi_icon = lv_label_create(s_wifi_btn);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_center(s_wifi_icon);

    /* Diagonal slash overlay – shown only when WiFi is disabled */
    s_wifi_slash = lv_label_create(s_wifi_btn);
    lv_label_set_text(s_wifi_slash, "/");
    lv_obj_set_style_text_color(s_wifi_slash, lv_color_hex(0xE94560), 0);
    lv_obj_center(s_wifi_slash);

    /* Apply initial (disabled) style */
    update_wifi_btn_style();

    /* List – fills remaining vertical space below the title */
    s_list = lv_list_create(s_screen);
    lv_coord_t list_h = (lv_coord_t)lv_disp_get_ver_res(lv_disp_get_default()) - 56;
    lv_obj_set_size(s_list, LV_PCT(100), list_h);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x16213E), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);

    /* Encoder group – governs keyboard / encoder focus */
    s_group = lv_group_create();
    lv_group_set_wrap(s_group, false); /* don't wrap at list ends */

    /*
     * Create a virtual encoder indev so LVGL routing works even when no
     * physical encoder hardware is connected to the ESP32.  The actual
     * movement is driven programmatically via lv_indev_send_event().
     */
    s_enc_indev = lv_indev_create();
    lv_indev_set_type(s_enc_indev, LV_INDEV_TYPE_ENCODER);
    /* No read callback: we drive events manually */
    lv_indev_set_group(s_enc_indev, s_group);

    lv_screen_load(s_screen);

    ESP_LOGI(TAG, "Songlist view created");
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * Rebuild the lv_list contents from s_songs[].
 * Must be called with the LVGL lock held.
 */
static void rebuild_list(void)
{
    /* Remove all existing children */
    lv_obj_clean(s_list);
    s_focused_idx = 0;

    for (uint8_t i = 0; i < s_song_count; i++) {
        lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_AUDIO, s_songs[i].name);

        /* Style: dark item background, large font, high-contrast text */
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0F3460), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xE94560), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 6, 0);

        /* Style the label child */
        lv_obj_t *lbl = lv_obj_get_child(btn, -1); /* last child = label */
        if (lbl) {
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xE0E0FF), 0);
            lv_obj_set_style_text_color(lbl, lv_color_white(), LV_STATE_FOCUSED);
        }

        /* Store song-ID in the button's user data */
        lv_obj_set_user_data(btn, (void *)(uintptr_t)s_songs[i].id);

        /* Touch / click event */
        lv_obj_add_event_cb(btn, on_list_item_clicked, LV_EVENT_CLICKED, NULL);

        /* Add to encoder group so it receives focus */
        lv_group_add_obj(s_group, btn);
    }

    /* Focus the first item */
    if (s_song_count > 0) {
        focus_item(0);
    }
}

/** Move focus to item at absolute index idx (clamped to valid range). */
static void focus_item(int16_t idx)
{
    if (s_song_count == 0) return;

    if (idx < 0) idx = 0;
    if (idx >= (int16_t)s_song_count) idx = (int16_t)s_song_count - 1;

    s_focused_idx = idx;

    /* Walk the group to the target object */
    lv_obj_t *target = lv_obj_get_child(s_list, idx);
    if (target) {
        lv_group_focus_obj(target);
        /* Scroll the list so the focused item is visible */
        lv_obj_scroll_to_view(target, LV_ANIM_ON);
    }
}

static void on_list_item_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint16_t song_id = (uint16_t)(uintptr_t)lv_obj_get_user_data(btn);
    ESP_LOGI(TAG, "Item clicked: song_id=%u", song_id);
    send_play_song(song_id);
}

/* =========================================================================
 * CMD_PLAY_SONG sender
 *
 * Payload layout (3 bytes):
 *   [song_id_lo : u8][song_id_hi : u8]
 * ========================================================================= */

static void send_play_song(uint16_t song_id)
{
    /* Auto-disable WiFi when a song is selected */
    if (s_wifi_enabled) {
        s_wifi_enabled = false;
        update_wifi_btn_style();
        uart_comm_send_wifi_ctrl(false);
        ESP_LOGI(TAG, "WiFi auto-disabled on song selection");
    }

    uint8_t payload[2];
    payload[0] = (uint8_t)(song_id & 0xFF);
    payload[1] = (uint8_t)(song_id >> 8);

    uint8_t checksum = CMD_PLAY_SONG ^ (uint8_t)sizeof(payload);
    for (int i = 0; i < (int)sizeof(payload); i++) {
        checksum ^= payload[i];
    }

    /* [MAGIC 8B][CMD 1B][LEN 1B][PAYLOAD 2B][CHECKSUM 1B] = 13 bytes */
    uint8_t pkt[13];
    memcpy(&pkt[0], UART_MAGIC_BYTES, 8);
    pkt[8]  = CMD_PLAY_SONG;
    pkt[9]  = (uint8_t)sizeof(payload);
    memcpy(&pkt[10], payload, sizeof(payload));
    pkt[12] = checksum;

    uart_write_bytes(UART_COMM_PORT, pkt, sizeof(pkt));
    ESP_LOGI(TAG, "CMD_PLAY_SONG sent: song_id=%u", song_id);
}

/* =========================================================================
 * lv_async_call callbacks – executed inside the LVGL task
 * ========================================================================= */

static void async_cb_update_list(void *user_data)
{
    async_songlist_payload_t *p = (async_songlist_payload_t *)user_data;

    s_song_count = 0;
    const uint8_t *ptr = p->data;
    const uint8_t *end = p->data + p->len;

    while (ptr + 2 <= end && s_song_count < SONGLIST_MAX_SONGS) {
        uint16_t song_id = (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
        ptr += 2;

        /* End-of-list: song_id == 0 and next byte is '\0' */
        if (song_id == 0 && ptr < end && *ptr == '\0') {
            break;
        }

        /* Copy null-terminated name */
        const uint8_t *name_start = ptr;
        while (ptr < end && *ptr != '\0') ptr++;
        if (ptr >= end) break; /* Malformed: no null terminator */

        size_t name_len = (size_t)(ptr - name_start);
        if (name_len >= MAX_SONG_NAME_LEN) name_len = MAX_SONG_NAME_LEN - 1;

        s_songs[s_song_count].id = song_id;
        memcpy(s_songs[s_song_count].name, name_start, name_len);
        s_songs[s_song_count].name[name_len] = '\0';
        s_song_count++;
        ptr++; /* skip '\0' terminator */
    }

    ESP_LOGI(TAG, "Song list updated: %u songs", s_song_count);
    rebuild_list();

    free(p->data);
    free(p);
}

static void async_cb_encoder_move(void *user_data)
{
    async_encoder_move_t *p = (async_encoder_move_t *)user_data;
    focus_item(s_focused_idx + p->steps);
    free(p);
}

static void async_cb_encoder_btn(void *user_data)
{
    (void)user_data;
    if (s_song_count == 0) return;

    lv_obj_t *focused = lv_group_get_focused(s_group);
    if (focused) {
        uint16_t song_id = (uint16_t)(uintptr_t)lv_obj_get_user_data(focused);
        ESP_LOGI(TAG, "Encoder button: selecting song_id=%u", song_id);
        send_play_song(song_id);
    }
}

/* =========================================================================
 * Public async bridges (called from UART task, Core 0)
 * ========================================================================= */

void ui_songlist_update_async(const uint8_t *data, uint16_t len)
{
    if (!s_screen) {
        ESP_LOGW(TAG, "update_async called before ui_songlist_create()");
        return;
    }

    async_songlist_payload_t *p = malloc(sizeof(async_songlist_payload_t));
    if (!p) {
        ESP_LOGE(TAG, "OOM in update_async");
        return;
    }
    p->data = malloc(len);
    if (!p->data) {
        ESP_LOGE(TAG, "OOM in update_async (data buffer)");
        free(p);
        return;
    }
    memcpy(p->data, data, len);
    p->len = len;

    lv_lock();
    lv_async_call(async_cb_update_list, p);
    lv_unlock();
}

void ui_songlist_encoder_move_async(int8_t steps)
{
    if (!s_screen) return;

    async_encoder_move_t *p = malloc(sizeof(async_encoder_move_t));
    if (!p) {
        ESP_LOGE(TAG, "OOM in encoder_move_async");
        return;
    }
    p->steps = steps;
    lv_lock();
    lv_async_call(async_cb_encoder_move, p);
    lv_unlock();
}

void ui_songlist_encoder_btn_async(void)
{
    if (!s_screen) return;
    lv_lock();
    lv_async_call(async_cb_encoder_btn, NULL);
    lv_unlock();
}

/* =========================================================================
 * Screen load helpers
 * ========================================================================= */

void ui_songlist_show(void)
{
    if (s_screen) {
        lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    }
}

static void async_cb_show_songlist(void *arg)
{
    (void)arg;
    ui_songlist_show();
}

void ui_songlist_show_async(void)
{
    lv_lock();
    lv_async_call(async_cb_show_songlist, NULL);
    lv_unlock();
}
