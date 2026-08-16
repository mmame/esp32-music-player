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
 * - All Display->Host commands are queued via uart_comm_send_*() and flushed
 *   in the next CMD_ACK response; no direct uart_write_bytes() calls are made.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* Per-song settings cache – populated on first gear-tap via CMD_SONG_SETTINGS */
typedef struct {
    bool    valid;               /* true once player has responded                */
    uint8_t flags;               /* bit0=loop, bit1=fixed_speed_en, bit3=dimmer_override */
    uint8_t fixed_speed_x100;    /* speed × 100 (e.g. 100 = 1.0×)                */
    uint8_t dimmer_max;          /* max brightness 0-100                          */
    uint8_t dimmer_min;          /* min brightness 0-100                          */
    uint8_t dimmer_rps_ref_x10;  /* full-brightness RPS × 10                     */
    uint8_t dimmer_holdoff_s;    /* seconds before dimmer activates               */
    uint8_t pitch_influence_pct; /* 0-100: 0=time-stretch, 100=tape effect        */
} song_settings_cache_t;

static song_entry_t         s_songs[SONGLIST_MAX_SONGS];
static song_settings_cache_t s_settings[SONGLIST_MAX_SONGS];
static uint8_t      s_song_count = 0;
static int16_t      s_focused_idx = 0;  /* currently focused list-button index */

/* LVGL objects */
static lv_obj_t   *s_screen      = NULL;
static lv_obj_t   *s_list        = NULL;
static lv_group_t *s_group       = NULL;
static lv_indev_t *s_enc_indev   = NULL; /* virtual encoder indev */

/* WiFi toggle button */
static lv_obj_t  *s_wifi_btn     = NULL;   /* clickable container */
static lv_obj_t  *s_wifi_icon    = NULL;   /* label: LV_SYMBOL_WIFI */
static lv_obj_t  *s_wifi_slash   = NULL;   /* "\/" overlay when disabled */
static bool       s_wifi_enabled = false;  /* starts disabled */
static lv_timer_t *s_wifi_timer  = NULL;   /* auto-disable after 15 min */

#define WIFI_TIMEOUT_MS  (15u * 60u * 1000u)

/* BT enable/disable button */
static lv_obj_t *s_bt_btn     = NULL;
static lv_obj_t *s_bt_icon    = NULL;
static bool      s_bt_enabled = false; /* starts disabled */

/* Settings dialog – at most one open at a time */
static lv_obj_t  *s_settings_overlay   = NULL; /* backdrop (NULL when not open) */
static uint16_t   s_settings_song_id   = 0;    /* song_id whose dialog is open  */
static lv_obj_t  *s_cb_loop            = NULL; /* checkbox objects inside dialog */
static lv_obj_t  *s_cb_fixed_speed     = NULL;
static lv_obj_t  *s_lbl_pitch_val      = NULL;
static uint8_t    s_pitch_influence    = 0u;   /* dialog pitch influence 0-100   */
static lv_obj_t  *s_lbl_speed_val      = NULL; /* speed value label               */
static uint8_t    s_speed_x100         = 100;  /* dialog speed × 100 (70–140)    */
/* Dimmer override dialog widgets */
static lv_obj_t  *s_cb_dimmer_override = NULL;
static lv_obj_t  *s_lbl_dmax_val       = NULL;
static lv_obj_t  *s_lbl_dmin_val       = NULL;
static lv_obj_t  *s_lbl_drps_val       = NULL;
static lv_obj_t  *s_lbl_dhld_val       = NULL;
static uint8_t    s_dimmer_max         = 100u;
static uint8_t    s_dimmer_min         = 0u;
static uint8_t    s_dimmer_rps_x10     = 14u; /* 1.4 rps default */
static uint8_t    s_dimmer_holdoff_s   = 0u;
/* Tab panels */
static lv_obj_t  *s_sound_panel        = NULL;
static lv_obj_t  *s_dimmer_panel       = NULL;
static lv_obj_t  *s_tab_sound_btn      = NULL;
static lv_obj_t  *s_tab_dimmer_btn     = NULL;

/* ---------- Forward declarations ----------------------------------------- */
static void on_list_item_clicked(lv_event_t *e);
static void on_wifi_btn_clicked(lv_event_t *e);
static void update_wifi_btn_style(void);
static void wifi_timeout_cb(lv_timer_t *t);
static void on_bt_btn_clicked(lv_event_t *e);
static void update_bt_btn_style(void);
static void update_dimmer_labels(void);
static void update_tab_style(bool sound_active);
static void send_play_song(uint16_t song_id);
static void focus_item(int16_t idx);
static void create_wifi_info_popup(void);
static void on_wifi_info_ok(lv_event_t *e);
static void create_settings_dialog(uint16_t song_id);
static void on_settings_ok(lv_event_t *e);
static void on_settings_cancel(lv_event_t *e);
static void send_play_song(uint16_t song_id);

/* ---------- async payload structs ---------------------------------------- */

typedef struct {
    uint8_t *data; /* heap-allocated; freed by the async callback */
    uint16_t len;
} async_songlist_payload_t;

typedef struct {
    uint16_t song_id;
    uint8_t  flags;
    uint8_t  fixed_speed_x100;
    uint8_t  dimmer_max;
    uint8_t  dimmer_min;
    uint8_t  dimmer_rps_ref_x10;
    uint8_t  dimmer_holdoff_s;
    uint8_t  pitch_influence_pct;
} async_song_settings_t;

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
        if (s_wifi_timer) { lv_timer_delete(s_wifi_timer); s_wifi_timer = NULL; }
        s_wifi_timer = lv_timer_create(wifi_timeout_cb, WIFI_TIMEOUT_MS, NULL);
        lv_timer_set_repeat_count(s_wifi_timer, 1);
        create_wifi_info_popup();
    } else {
        if (s_wifi_timer) { lv_timer_delete(s_wifi_timer); s_wifi_timer = NULL; }
    }
}

static void wifi_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_wifi_timer = NULL; /* auto-deleted by LVGL after repeat_count reaches 0 */
    s_wifi_enabled = false;
    update_wifi_btn_style();
    uart_comm_send_wifi_ctrl(false);
    ESP_LOGI(TAG, "WiFi auto-disabled after 15-minute timeout");
}

/* =========================================================================
 * BT button helpers
 * ========================================================================= */

static void update_bt_btn_style(void)
{
    if (s_bt_enabled) {
        lv_obj_set_style_bg_color(s_bt_btn, lv_color_hex(0x00B4D8), 0);
        lv_obj_set_style_bg_opa(s_bt_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_bt_icon, lv_color_white(), 0);
    } else {
        lv_obj_set_style_bg_color(s_bt_btn, lv_color_hex(0x2A2A3E), 0);
        lv_obj_set_style_bg_opa(s_bt_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_bt_icon, lv_color_hex(0x505060), 0);
    }
}

static void on_bt_btn_clicked(lv_event_t *e)
{
    (void)e;
    s_bt_enabled = !s_bt_enabled;
    update_bt_btn_style();
    uart_comm_send_bt_ctrl(s_bt_enabled);
    ESP_LOGI(TAG, "BT icon toggled: %s", s_bt_enabled ? "ENABLE" : "DISABLE");
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
    lv_obj_set_size(box, 520, 390);
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
        "1. Connect to WiFi:\n"
        "        \"MusicPlayer\"\n"
        "   Password: Crank!837\n"
        "\n"
        "2. Open in your browser:\n"
        "        192.168.4.1\n"
        "\n"
        "Auto-disables after 15 minutes.");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
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

    /* BT toggle button – left of the WiFi button */
    s_bt_btn = lv_obj_create(s_screen);
    lv_obj_set_size(s_bt_btn, 46, 44);
    lv_obj_align(s_bt_btn, LV_ALIGN_TOP_RIGHT, -58, 6);
    lv_obj_set_style_border_width(s_bt_btn, 0, 0);
    lv_obj_set_style_radius(s_bt_btn, 8, 0);
    lv_obj_set_style_pad_all(s_bt_btn, 0, 0);
    lv_obj_clear_flag(s_bt_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_bt_btn, on_bt_btn_clicked, LV_EVENT_CLICKED, NULL);
    s_bt_icon = lv_label_create(s_bt_btn);
    lv_label_set_text(s_bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_bt_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(s_bt_icon);
    update_bt_btn_style();

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
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(s_wifi_icon);

    /* Diagonal slash overlay – shown only when WiFi is disabled */
    s_wifi_slash = lv_label_create(s_wifi_btn);
    lv_label_set_text(s_wifi_slash, "/");
    lv_obj_set_style_text_font(s_wifi_slash, &lv_font_montserrat_28, 0);
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
    /* Ignore clicks that originated from the gear icon child */
    lv_obj_t *origin = lv_event_get_current_target(e);
    if (origin != btn) return;
    uint16_t song_id = (uint16_t)(uintptr_t)lv_obj_get_user_data(btn);
    ESP_LOGI(TAG, "Item clicked: song_id=%u", song_id);
    send_play_song(song_id);
}

/* =========================================================================
 * Settings dialog
 * ========================================================================= */

static void update_speed_label(void)
{
    if (!s_lbl_speed_val) return;
    char buf[10];
    snprintf(buf, sizeof(buf), "%.2fx", (float)s_speed_x100 / 100.0f);
    lv_label_set_text(s_lbl_speed_val, buf);
}

static void on_speed_minus(lv_event_t *e)
{
    (void)e;
    if (s_speed_x100 > 70u) s_speed_x100 -= 5u;
    if (s_cb_fixed_speed) lv_obj_add_state(s_cb_fixed_speed, LV_STATE_CHECKED);
    update_speed_label();
}

static void on_speed_plus(lv_event_t *e)
{
    (void)e;
    if (s_speed_x100 < 140u) s_speed_x100 += 5u;
    if (s_cb_fixed_speed) lv_obj_add_state(s_cb_fixed_speed, LV_STATE_CHECKED);
    update_speed_label();
}

static void update_pitch_label(void)
{
    if (!s_lbl_pitch_val) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", s_pitch_influence);
    lv_label_set_text(s_lbl_pitch_val, buf);
}

static void on_pitch_minus(lv_event_t *e) { (void)e; if (s_pitch_influence >= 5u) s_pitch_influence -= 5u; else s_pitch_influence = 0u; update_pitch_label(); }
static void on_pitch_plus (lv_event_t *e) { (void)e; if (s_pitch_influence <= 95u) s_pitch_influence += 5u; else s_pitch_influence = 100u; update_pitch_label(); }

/* ── Dimmer field helpers ─────────────────────────────────────────────────── */

static void update_dimmer_labels(void)
{
    char buf[16];
    if (s_lbl_dmax_val) {
        snprintf(buf, sizeof(buf), "%u%%", s_dimmer_max);
        lv_label_set_text(s_lbl_dmax_val, buf);
    }
    if (s_lbl_dmin_val) {
        snprintf(buf, sizeof(buf), "%u%%", s_dimmer_min);
        lv_label_set_text(s_lbl_dmin_val, buf);
    }
    if (s_lbl_drps_val) {
        snprintf(buf, sizeof(buf), "%.1f", (float)s_dimmer_rps_x10 / 10.0f);
        lv_label_set_text(s_lbl_drps_val, buf);
    }
    if (s_lbl_dhld_val) {
        snprintf(buf, sizeof(buf), "%us", s_dimmer_holdoff_s);
        lv_label_set_text(s_lbl_dhld_val, buf);
    }
}

static void on_dmax_minus(lv_event_t *e) { (void)e; if (s_dimmer_max > 0u)   s_dimmer_max   -= 5u; if (s_dimmer_max < s_dimmer_min) s_dimmer_max = s_dimmer_min; if (s_cb_dimmer_override) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED); update_dimmer_labels(); }
static void on_dmax_plus (lv_event_t *e) { (void)e; if (s_dimmer_max < 100u) s_dimmer_max   += 5u; if (s_dimmer_max > 100u) s_dimmer_max = 100u;             if (s_cb_dimmer_override) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED); update_dimmer_labels(); }
static void on_dmin_minus(lv_event_t *e) { (void)e; if (s_dimmer_min > 0u)   s_dimmer_min   -= 5u;                                                            if (s_cb_dimmer_override) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED); update_dimmer_labels(); }
static void on_dmin_plus (lv_event_t *e) { (void)e; if (s_dimmer_min < 100u) s_dimmer_min   += 5u; if (s_dimmer_min > s_dimmer_max) s_dimmer_min = s_dimmer_max; if (s_cb_dimmer_override) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED); update_dimmer_labels(); }
static void on_drps_minus(lv_event_t *e) { (void)e; if (s_dimmer_rps_x10 > 1u) s_dimmer_rps_x10 -= 1u;                                                        if (s_cb_dimmer_override) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED); update_dimmer_labels(); }
static void on_drps_plus (lv_event_t *e) { (void)e; if (s_dimmer_rps_x10 < 30u) s_dimmer_rps_x10 += 1u;                                                       if (s_cb_dimmer_override) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED); update_dimmer_labels(); }
static void on_dhld_minus(lv_event_t *e) { (void)e; if (s_dimmer_holdoff_s > 0u)   s_dimmer_holdoff_s -= 1u; update_dimmer_labels(); }
static void on_dhld_plus (lv_event_t *e) { (void)e; if (s_dimmer_holdoff_s < 255u) s_dimmer_holdoff_s += 1u; update_dimmer_labels(); }

static void update_tab_style(bool sound_active)
{
    if (s_tab_sound_btn)
        lv_obj_set_style_bg_color(s_tab_sound_btn,
            sound_active ? lv_color_hex(0x1E88E5) : lv_color_hex(0x2A2A4A), 0);
    if (s_tab_dimmer_btn)
        lv_obj_set_style_bg_color(s_tab_dimmer_btn,
            sound_active ? lv_color_hex(0x2A2A4A) : lv_color_hex(0x1E88E5), 0);
}

static void on_tab_sound(lv_event_t *e)
{
    (void)e;
    if (s_sound_panel)  lv_obj_clear_flag(s_sound_panel,  LV_OBJ_FLAG_HIDDEN);
    if (s_dimmer_panel) lv_obj_add_flag(s_dimmer_panel,   LV_OBJ_FLAG_HIDDEN);
    update_tab_style(true);
}

static void on_tab_dimmer(lv_event_t *e)
{
    (void)e;
    if (s_sound_panel)  lv_obj_add_flag(s_sound_panel,    LV_OBJ_FLAG_HIDDEN);
    if (s_dimmer_panel) lv_obj_clear_flag(s_dimmer_panel, LV_OBJ_FLAG_HIDDEN);
    update_tab_style(false);
}

static void on_settings_cancel(lv_event_t *e)
{
    (void)e;
    if (s_settings_overlay) {
        lv_obj_delete(s_settings_overlay);
        s_settings_overlay = NULL;
        s_settings_song_id = 0;
        s_cb_loop          = NULL;
        s_cb_fixed_speed   = NULL;
        s_lbl_pitch_val    = NULL;
        s_lbl_speed_val    = NULL;
        s_cb_dimmer_override = NULL;
        s_lbl_dmax_val     = NULL;
        s_lbl_dmin_val     = NULL;
        s_lbl_drps_val     = NULL;
        s_lbl_dhld_val     = NULL;
        s_sound_panel      = NULL;
        s_dimmer_panel     = NULL;
        s_tab_sound_btn    = NULL;
        s_tab_dimmer_btn   = NULL;
    }
}

static void on_settings_ok(lv_event_t *e)
{
    (void)e;
    if (!s_settings_overlay) return;

    uint16_t song_id = s_settings_song_id;

    /* Collect checkbox states */
    bool loop_en      = (lv_obj_get_state(s_cb_loop)        & LV_STATE_CHECKED) != 0;
    bool speed_en     = (lv_obj_get_state(s_cb_fixed_speed) & LV_STATE_CHECKED) != 0;
    bool dimmer_ov_en = s_cb_dimmer_override
                        ? ((lv_obj_get_state(s_cb_dimmer_override) & LV_STATE_CHECKED) != 0)
                        : false;

    uint8_t flags = 0;
    if (loop_en)      flags |= 0x01u;
    if (speed_en)     flags |= 0x02u;
    if (dimmer_ov_en) flags |= 0x08u;
    uint8_t fixed_speed_x100 = speed_en ? s_speed_x100 : 100u;
    uint8_t d_max  = dimmer_ov_en ? s_dimmer_max       : 100u;
    uint8_t d_min  = dimmer_ov_en ? s_dimmer_min       : 0u;
    uint8_t d_rps  = dimmer_ov_en ? s_dimmer_rps_x10   : 14u;
    uint8_t d_hld  = s_dimmer_holdoff_s;

    /* Update local cache */
    uint8_t cache_idx = (uint8_t)(song_id - 1);
    if (cache_idx < SONGLIST_MAX_SONGS) {
        s_settings[cache_idx].flags               = flags;
        s_settings[cache_idx].fixed_speed_x100    = fixed_speed_x100;
        s_settings[cache_idx].dimmer_max          = d_max;
        s_settings[cache_idx].dimmer_min          = d_min;
        s_settings[cache_idx].dimmer_rps_ref_x10  = d_rps;
        s_settings[cache_idx].dimmer_holdoff_s    = d_hld;
        s_settings[cache_idx].pitch_influence_pct = s_pitch_influence;
        s_settings[cache_idx].valid               = true;
    }

    /* Send to player and request fresh response so other views update */
    uart_comm_send_set_song_settings(song_id, flags, fixed_speed_x100,
                                     d_max, d_min, d_rps, d_hld, s_pitch_influence);
    uart_comm_send_song_settings_req(song_id);
    ESP_LOGI(TAG, "Settings saved: song_id=%u flags=0x%02X", song_id, flags);

    /* Close dialog */
    lv_obj_delete(s_settings_overlay);
    s_settings_overlay   = NULL;
    s_settings_song_id   = 0;
    s_cb_loop            = NULL;
    s_cb_fixed_speed     = NULL;
    s_lbl_pitch_val      = NULL;
    s_lbl_speed_val      = NULL;
    s_cb_dimmer_override = NULL;
    s_lbl_dmax_val       = NULL;
    s_lbl_dmin_val       = NULL;
    s_lbl_drps_val       = NULL;
    s_lbl_dhld_val       = NULL;
    s_sound_panel        = NULL;
    s_dimmer_panel       = NULL;
    s_tab_sound_btn      = NULL;
    s_tab_dimmer_btn     = NULL;
}

static void create_settings_dialog(uint16_t song_id)
{
    /* Only one dialog at a time */
    if (s_settings_overlay) {
        lv_obj_delete(s_settings_overlay);
        s_settings_overlay = NULL;
    }

    /* Find song name */
    const char *song_name = "";
    for (uint8_t i = 0; i < s_song_count; i++) {
        if (s_songs[i].id == song_id) { song_name = s_songs[i].name; break; }
    }

    /* Request fresh settings from player (response updates the cache) */
    uart_comm_send_song_settings_req(song_id);

    /* Determine current values from cache (defaults until response arrives) */
    uint8_t cache_idx = (uint8_t)(song_id - 1);
    bool    cached_loop         = false;
    bool    cached_speed        = false;
    uint8_t cached_pitch_influence = 0u;
    bool    cached_dimmer_ov    = false;
    uint8_t cached_spd_x100     = 100u;
    uint8_t cached_dmax         = 100u;
    uint8_t cached_dmin         = 0u;
    uint8_t cached_drps         = 14u;
    uint8_t cached_dhld         = 0u;
    if (cache_idx < SONGLIST_MAX_SONGS && s_settings[cache_idx].valid) {
        cached_loop         = (s_settings[cache_idx].flags & 0x01u) != 0;
        cached_speed        = (s_settings[cache_idx].flags & 0x02u) != 0;
        cached_pitch_influence = s_settings[cache_idx].pitch_influence_pct;
        cached_dimmer_ov    = (s_settings[cache_idx].flags & 0x08u) != 0;
        cached_spd_x100     = s_settings[cache_idx].fixed_speed_x100;
        if (cached_spd_x100 < 70u || cached_spd_x100 > 140u) cached_spd_x100 = 100u;
        cached_dmax = s_settings[cache_idx].dimmer_max;
        cached_dmin = s_settings[cache_idx].dimmer_min;
        cached_drps = s_settings[cache_idx].dimmer_rps_ref_x10;
        if (cached_drps == 0u) cached_drps = 14u;
        cached_dhld = s_settings[cache_idx].dimmer_holdoff_s;
    }
    s_pitch_influence  = cached_pitch_influence;
    s_speed_x100       = cached_spd_x100;
    s_dimmer_max       = cached_dmax;
    s_dimmer_min       = cached_dmin;
    s_dimmer_rps_x10   = cached_drps;
    s_dimmer_holdoff_s = cached_dhld;

    s_settings_song_id = song_id;

    /* ── Backdrop ─────────────────────────────────────────────────── */
    s_settings_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_settings_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_settings_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_settings_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_settings_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_settings_overlay, 0, 0);
    lv_obj_clear_flag(s_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Tap on backdrop = cancel */
    lv_obj_add_event_cb(s_settings_overlay, on_settings_cancel, LV_EVENT_CLICKED, NULL);

    /* ── Dialog box ───────────────────────────────────────────────── */
    lv_obj_t *box = lv_obj_create(s_settings_overlay);
    lv_obj_set_size(box, 500, 450);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

    /* ── Title ────────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text_fmt(title, LV_SYMBOL_SETTINGS "  %s", song_name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1E88E5), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, 452);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* ── Divider ──────────────────────────────────────────────────── */
    lv_obj_t *line = lv_obj_create(box);
    lv_obj_set_size(line, 452, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x2A3A5A), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 36);

    /* ── Tab buttons: [Sound] [Dimmer] ───────────────────────────── */
    s_tab_sound_btn = lv_button_create(box);
    lv_obj_set_size(s_tab_sound_btn, 222, 36);
    lv_obj_align(s_tab_sound_btn, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_set_style_bg_color(s_tab_sound_btn, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_bg_color(s_tab_sound_btn, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_tab_sound_btn, 8, 0);
    lv_obj_set_style_border_width(s_tab_sound_btn, 0, 0);
    lv_obj_add_event_cb(s_tab_sound_btn, on_tab_sound, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ts = lv_label_create(s_tab_sound_btn);
    lv_label_set_text(lbl_ts, "Sound");
    lv_obj_set_style_text_font(lbl_ts, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ts, lv_color_white(), 0);
    lv_obj_center(lbl_ts);

    s_tab_dimmer_btn = lv_button_create(box);
    lv_obj_set_size(s_tab_dimmer_btn, 222, 36);
    lv_obj_align(s_tab_dimmer_btn, LV_ALIGN_TOP_RIGHT, 0, 44);
    lv_obj_set_style_bg_color(s_tab_dimmer_btn, lv_color_hex(0x2A2A4A), 0);
    lv_obj_set_style_bg_color(s_tab_dimmer_btn, lv_color_hex(0x3A3A5A), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_tab_dimmer_btn, 8, 0);
    lv_obj_set_style_border_width(s_tab_dimmer_btn, 0, 0);
    lv_obj_add_event_cb(s_tab_dimmer_btn, on_tab_dimmer, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_td = lv_label_create(s_tab_dimmer_btn);
    lv_label_set_text(lbl_td, "Dimmer");
    lv_obj_set_style_text_font(lbl_td, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_td, lv_color_white(), 0);
    lv_obj_center(lbl_td);

    /* ── Shared panel geometry: box content = 452×402, panels at y=88, h=256 ── */

    /* Sound panel (visible by default) */
    s_sound_panel = lv_obj_create(box);
    lv_obj_set_size(s_sound_panel, 452, 256);
    lv_obj_align(s_sound_panel, LV_ALIGN_TOP_LEFT, 0, 88);
    lv_obj_set_style_bg_opa(s_sound_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sound_panel, 0, 0);
    lv_obj_set_style_pad_all(s_sound_panel, 0, 0);
    lv_obj_clear_flag(s_sound_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Dimmer panel (hidden by default) */
    s_dimmer_panel = lv_obj_create(box);
    lv_obj_set_size(s_dimmer_panel, 452, 256);
    lv_obj_align(s_dimmer_panel, LV_ALIGN_TOP_LEFT, 0, 88);
    lv_obj_set_style_bg_opa(s_dimmer_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dimmer_panel, 0, 0);
    lv_obj_set_style_pad_all(s_dimmer_panel, 0, 0);
    lv_obj_clear_flag(s_dimmer_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dimmer_panel, LV_OBJ_FLAG_HIDDEN);

    /* ── Sound panel content ──────────────────────────────────────── */
    s_cb_loop = lv_checkbox_create(s_sound_panel);
    lv_checkbox_set_text(s_cb_loop, "Loop");
    if (cached_loop) lv_obj_add_state(s_cb_loop, LV_STATE_CHECKED);
    lv_obj_set_style_text_font(s_cb_loop, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cb_loop, lv_color_hex(0xE0E0FF), 0);
    lv_obj_align(s_cb_loop, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cb_fixed_speed = lv_checkbox_create(s_sound_panel);
    lv_checkbox_set_text(s_cb_fixed_speed, "Fixed speed");
    if (cached_speed) lv_obj_add_state(s_cb_fixed_speed, LV_STATE_CHECKED);
    lv_obj_set_style_text_font(s_cb_fixed_speed, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cb_fixed_speed, lv_color_hex(0xE0E0FF), 0);
    lv_obj_align(s_cb_fixed_speed, LV_ALIGN_TOP_LEFT, 0, 36);

    /* Speed row: [-] 1.00x [+] compact at y=72 */
    {
        /* row = 36+8+90+8+36 = 178 px, centred in 452 */
        const int16_t SX = (452 - 178) / 2;  /* = 137 */
        lv_obj_t *bm = lv_button_create(s_sound_panel);
        lv_obj_set_size(bm, 36, 36);
        lv_obj_align(bm, LV_ALIGN_TOP_LEFT, SX, 72);
        lv_obj_set_style_bg_color(bm, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_bg_color(bm, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
        lv_obj_set_style_radius(bm, 6, 0);
        lv_obj_set_style_border_width(bm, 0, 0);
        lv_obj_add_event_cb(bm, on_speed_minus, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lm = lv_label_create(bm);
        lv_label_set_text(lm, "-");
        lv_obj_set_style_text_font(lm, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lm, lv_color_white(), 0);
        lv_obj_center(lm);

        s_lbl_speed_val = lv_label_create(s_sound_panel);
        lv_obj_set_size(s_lbl_speed_val, 90, 36);
        lv_obj_align(s_lbl_speed_val, LV_ALIGN_TOP_LEFT, SX + 44, 72);
        lv_obj_set_style_text_font(s_lbl_speed_val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_lbl_speed_val, lv_color_hex(0xE0E0FF), 0);
        lv_obj_set_style_text_align(s_lbl_speed_val, LV_TEXT_ALIGN_CENTER, 0);
        update_speed_label();

        lv_obj_t *bp = lv_button_create(s_sound_panel);
        lv_obj_set_size(bp, 36, 36);
        lv_obj_align(bp, LV_ALIGN_TOP_LEFT, SX + 44 + 90 + 8, 72);
        lv_obj_set_style_bg_color(bp, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_bg_color(bp, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
        lv_obj_set_style_radius(bp, 6, 0);
        lv_obj_set_style_border_width(bp, 0, 0);
        lv_obj_add_event_cb(bp, on_speed_plus, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lp = lv_label_create(bp);
        lv_label_set_text(lp, "+");
        lv_obj_set_style_text_font(lp, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lp, lv_color_white(), 0);
        lv_obj_center(lp);
    }

    {
        lv_obj_t *lbl_p = lv_label_create(s_sound_panel);
        lv_label_set_text(lbl_p, "Pitch influence");
        lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl_p, lv_color_hex(0xE0E0FF), 0);
        lv_obj_align(lbl_p, LV_ALIGN_TOP_LEFT, 0, 124);
        lv_obj_t *bm_p = lv_button_create(s_sound_panel);
        lv_obj_set_size(bm_p, 36, 36);
        lv_obj_align(bm_p, LV_ALIGN_TOP_LEFT, 200, 116);
        lv_obj_set_style_bg_color(bm_p, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_bg_color(bm_p, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
        lv_obj_set_style_radius(bm_p, 6, 0);
        lv_obj_set_style_border_width(bm_p, 0, 0);
        lv_obj_add_event_cb(bm_p, on_pitch_minus, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lm_p = lv_label_create(bm_p);
        lv_label_set_text(lm_p, "-");
        lv_obj_set_style_text_font(lm_p, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lm_p, lv_color_white(), 0);
        lv_obj_center(lm_p);
        s_lbl_pitch_val = lv_label_create(s_sound_panel);
        lv_obj_set_size(s_lbl_pitch_val, 72, 36);
        lv_obj_align(s_lbl_pitch_val, LV_ALIGN_TOP_LEFT, 240, 116);
        lv_obj_set_style_text_font(s_lbl_pitch_val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_lbl_pitch_val, lv_color_hex(0xE0E0FF), 0);
        lv_obj_set_style_text_align(s_lbl_pitch_val, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *bp_p = lv_button_create(s_sound_panel);
        lv_obj_set_size(bp_p, 36, 36);
        lv_obj_align(bp_p, LV_ALIGN_TOP_LEFT, 316, 116);
        lv_obj_set_style_bg_color(bp_p, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_bg_color(bp_p, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
        lv_obj_set_style_radius(bp_p, 6, 0);
        lv_obj_set_style_border_width(bp_p, 0, 0);
        lv_obj_add_event_cb(bp_p, on_pitch_plus, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lp_p = lv_label_create(bp_p);
        lv_label_set_text(lp_p, "+");
        lv_obj_set_style_text_font(lp_p, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lp_p, lv_color_white(), 0);
        lv_obj_center(lp_p);
        update_pitch_label();
    }

    /* ── Dimmer panel content ─────────────────────────────────────── */
    s_cb_dimmer_override = lv_checkbox_create(s_dimmer_panel);
    lv_checkbox_set_text(s_cb_dimmer_override, "Custom dimmer");
    if (cached_dimmer_ov) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED);
    lv_obj_set_style_text_font(s_cb_dimmer_override, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cb_dimmer_override, lv_color_hex(0xFFD060), 0);
    lv_obj_align(s_cb_dimmer_override, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Spinner row macro: creates label + [-] + value label + [+] inside parent */
#define MAKE_DIMMER_ROW(par, ypos, lbl_txt, on_m, on_p, lbl_ref)                    \
    do {                                                                              \
        lv_obj_t *_rl = lv_label_create(par);                                        \
        lv_label_set_text(_rl, lbl_txt);                                              \
        lv_obj_set_style_text_font(_rl, &lv_font_montserrat_20, 0);                  \
        lv_obj_set_style_text_color(_rl, lv_color_hex(0xA0A0C0), 0);                 \
        lv_obj_align(_rl, LV_ALIGN_TOP_LEFT, 0, (ypos) + 8);                         \
        lv_obj_t *_rm = lv_button_create(par);                                        \
        lv_obj_set_size(_rm, 36, 36);                                                 \
        lv_obj_align(_rm, LV_ALIGN_TOP_LEFT, 200, (ypos));                            \
        lv_obj_set_style_bg_color(_rm, lv_color_hex(0x1E88E5), 0);                   \
        lv_obj_set_style_bg_color(_rm, lv_color_hex(0x1565C0), LV_STATE_PRESSED);    \
        lv_obj_set_style_radius(_rm, 6, 0);                                           \
        lv_obj_set_style_border_width(_rm, 0, 0);                                     \
        lv_obj_add_event_cb(_rm, on_m, LV_EVENT_CLICKED, NULL);                       \
        lv_obj_t *_rml = lv_label_create(_rm);                                        \
        lv_label_set_text(_rml, "-");                                                  \
        lv_obj_set_style_text_font(_rml, &lv_font_montserrat_20, 0);                  \
        lv_obj_set_style_text_color(_rml, lv_color_white(), 0);                       \
        lv_obj_center(_rml);                                                           \
        (lbl_ref) = lv_label_create(par);                                              \
        lv_obj_set_size((lbl_ref), 72, 36);                                            \
        lv_obj_align((lbl_ref), LV_ALIGN_TOP_LEFT, 240, (ypos));                       \
        lv_obj_set_style_text_font((lbl_ref), &lv_font_montserrat_20, 0);              \
        lv_obj_set_style_text_color((lbl_ref), lv_color_hex(0xE0E0FF), 0);             \
        lv_obj_set_style_text_align((lbl_ref), LV_TEXT_ALIGN_CENTER, 0);               \
        lv_obj_t *_rp = lv_button_create(par);                                        \
        lv_obj_set_size(_rp, 36, 36);                                                  \
        lv_obj_align(_rp, LV_ALIGN_TOP_LEFT, 316, (ypos));                             \
        lv_obj_set_style_bg_color(_rp, lv_color_hex(0x1E88E5), 0);                    \
        lv_obj_set_style_bg_color(_rp, lv_color_hex(0x1565C0), LV_STATE_PRESSED);     \
        lv_obj_set_style_radius(_rp, 6, 0);                                            \
        lv_obj_set_style_border_width(_rp, 0, 0);                                      \
        lv_obj_add_event_cb(_rp, on_p, LV_EVENT_CLICKED, NULL);                        \
        lv_obj_t *_rpl = lv_label_create(_rp);                                        \
        lv_label_set_text(_rpl, "+");                                                  \
        lv_obj_set_style_text_font(_rpl, &lv_font_montserrat_20, 0);                  \
        lv_obj_set_style_text_color(_rpl, lv_color_white(), 0);                        \
        lv_obj_center(_rpl);                                                           \
    } while(0)

    MAKE_DIMMER_ROW(s_dimmer_panel,  42, "Max bright",  on_dmax_minus, on_dmax_plus, s_lbl_dmax_val);
    MAKE_DIMMER_ROW(s_dimmer_panel,  84, "Min bright",  on_dmin_minus, on_dmin_plus, s_lbl_dmin_val);
    MAKE_DIMMER_ROW(s_dimmer_panel, 126, "Full at RPS", on_drps_minus, on_drps_plus, s_lbl_drps_val);
    MAKE_DIMMER_ROW(s_dimmer_panel, 168, "Holdoff",     on_dhld_minus, on_dhld_plus, s_lbl_dhld_val);
    update_dimmer_labels();

#undef MAKE_DIMMER_ROW

    /* ── Cancel / OK ─────────────────────────────────────────────── */
    lv_obj_t *btn_cancel = lv_obj_create(box);
    lv_obj_set_size(btn_cancel, 180, 44);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x2A2A3E), 0);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_cancel, lv_color_hex(0x505060), 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x1A1A2E), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_cancel, on_settings_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xA0A0C0), 0);
    lv_obj_center(lbl_cancel);

    lv_obj_t *btn_ok = lv_obj_create(box);
    lv_obj_set_size(btn_ok, 180, 44);
    lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_bg_opa(btn_ok, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_ok, 0, 0);
    lv_obj_set_style_radius(btn_ok, 8, 0);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_ok, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_ok, on_settings_ok, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, "OK");
    lv_obj_set_style_text_font(lbl_ok, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ok, lv_color_white(), 0);
    lv_obj_center(lbl_ok);
}

void ui_songlist_open_settings_dialog(uint16_t song_id)
{
    if (song_id == 0) return;
    create_settings_dialog(song_id);
}

uint16_t ui_songlist_find_song_id_by_name(const char *name)
{
    if (!name) return 0;
    for (uint8_t i = 0; i < s_song_count; i++) {
        if (strcmp(s_songs[i].name, name) == 0) return s_songs[i].id;
    }
    return 0;
}

bool ui_songlist_get_song_name(uint16_t song_id, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return false;
    for (uint8_t i = 0; i < s_song_count; i++) {
        if (s_songs[i].id == song_id) {
            strlcpy(buf, s_songs[i].name, buf_len);
            return true;
        }
    }
    return false;
}

uint16_t ui_songlist_get_next_song_id(uint16_t current_id)
{
    if (s_song_count == 0) return 0;
    for (uint8_t i = 0; i < s_song_count; i++) {
        if (s_songs[i].id == current_id) {
            return s_songs[(i + 1) % s_song_count].id;
        }
    }
    return s_songs[0].id;
}

static void send_play_song(uint16_t song_id)
{
    uart_comm_send_play_song(song_id);
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
        for (char *c = s_songs[s_song_count].name; *c; c++) { if (*c == '_') *c = ' '; }
        s_song_count++;
        ptr++; /* skip '\0' terminator */
    }

    ESP_LOGI(TAG, "Song list updated: %u songs", s_song_count);
    /* Invalidate the settings cache whenever the song list changes */
    for (int i = 0; i < SONGLIST_MAX_SONGS; i++) {
        s_settings[i].valid = false;
    }
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

/* =========================================================================
 * Song-settings async delivery (UART task → LVGL task)
 * ========================================================================= */

static void async_cb_song_settings(void *user_data)
{
    async_song_settings_t *p = (async_song_settings_t *)user_data;

    /* Update the cache */
    uint8_t idx = (uint8_t)(p->song_id - 1);
    if (p->song_id > 0 && idx < SONGLIST_MAX_SONGS) {
        s_settings[idx].flags               = p->flags;
        s_settings[idx].fixed_speed_x100    = p->fixed_speed_x100;
        s_settings[idx].dimmer_max          = p->dimmer_max;
        s_settings[idx].dimmer_min          = p->dimmer_min;
        s_settings[idx].dimmer_rps_ref_x10  = p->dimmer_rps_ref_x10;
        s_settings[idx].dimmer_holdoff_s    = p->dimmer_holdoff_s;
        s_settings[idx].pitch_influence_pct = p->pitch_influence_pct;
        s_settings[idx].valid               = true;
    }

    /* If the dialog for this song is currently open, refresh the checkboxes */
    if (s_settings_overlay && s_settings_song_id == p->song_id) {
        if (s_cb_loop) {
            if (p->flags & 0x01u) lv_obj_add_state(s_cb_loop, LV_STATE_CHECKED);
            else                  lv_obj_clear_state(s_cb_loop, LV_STATE_CHECKED);
        }
        if (s_cb_fixed_speed) {
            if (p->flags & 0x02u) lv_obj_add_state(s_cb_fixed_speed, LV_STATE_CHECKED);
            else                  lv_obj_clear_state(s_cb_fixed_speed, LV_STATE_CHECKED);
        }
        if (s_lbl_pitch_val) {
            s_pitch_influence = p->pitch_influence_pct;
            update_pitch_label();
        }
        if (s_cb_dimmer_override) {
            if (p->flags & 0x08u) lv_obj_add_state(s_cb_dimmer_override, LV_STATE_CHECKED);
            else                  lv_obj_clear_state(s_cb_dimmer_override, LV_STATE_CHECKED);
        }
        if (s_lbl_speed_val) {
            uint8_t spd = p->fixed_speed_x100;
            if (spd < 70u || spd > 140u) spd = 100u;
            s_speed_x100 = spd;
            update_speed_label();
        }
        s_dimmer_max       = p->dimmer_max;
        s_dimmer_min       = p->dimmer_min;
        s_dimmer_rps_x10   = p->dimmer_rps_ref_x10 ? p->dimmer_rps_ref_x10 : 14u;
        s_dimmer_holdoff_s = p->dimmer_holdoff_s;
        update_dimmer_labels();
    }

    free(p);
}

void ui_songlist_song_settings_async(uint16_t song_id, uint8_t flags, uint8_t fixed_speed_x100,
                                     uint8_t dimmer_max, uint8_t dimmer_min,
                                     uint8_t dimmer_rps_ref_x10, uint8_t dimmer_holdoff_s,
                                     uint8_t pitch_influence_pct)
{
    if (!s_screen) return;

    async_song_settings_t *p = malloc(sizeof(async_song_settings_t));
    if (!p) {
        ESP_LOGE(TAG, "OOM in song_settings_async");
        return;
    }
    p->song_id              = song_id;
    p->flags                = flags;
    p->fixed_speed_x100     = fixed_speed_x100;
    p->dimmer_max           = dimmer_max;
    p->dimmer_min           = dimmer_min;
    p->dimmer_rps_ref_x10   = dimmer_rps_ref_x10;
    p->dimmer_holdoff_s     = dimmer_holdoff_s;
    p->pitch_influence_pct  = pitch_influence_pct;

    lv_lock();
    lv_async_call(async_cb_song_settings, p);
    lv_unlock();
}

static void async_cb_update_bt_enabled(void *user_data)
{
    bool enabled = (bool)(uintptr_t)user_data;
    s_bt_enabled = enabled;
    if (s_bt_btn) update_bt_btn_style();
}

void ui_songlist_update_bt_enabled_async(bool enabled)
{
    if (!s_screen) return;
    lv_lock();
    lv_async_call(async_cb_update_bt_enabled, (void *)(uintptr_t)enabled);
    lv_unlock();
}
