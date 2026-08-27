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
    uint8_t flags;               /* bit0=loop, bit1=fixed_speed_en, bit2=autoplay_next */
    uint8_t fixed_speed_x100;    /* speed × 100 (e.g. 100 = 1.0×)                */
    uint8_t dimmer_max;          /* max brightness 0-100                          */
    uint8_t dimmer_min;          /* min brightness 0-100                          */
    uint8_t dimmer_rps_ref_x10;  /* full-brightness RPS × 10                     */
    uint8_t dimmer_holdoff_s;    /* song-position timestamp before dimmer activates */
    uint8_t dimmer_fadein_s;     /* seconds to fade from 0 to full after holdoff  */
    uint8_t pitch_influence_pct; /* 0-100: 0=time-stretch, 100=tape effect        */
    uint8_t downmix_mode;        /* 0=mix, 1=ch1, 2=ch2                            */
    uint8_t downmix_fade_s;      /* 0-10 s transition when downmix changes         */
} song_settings_cache_t;

static song_entry_t         s_songs[SONGLIST_MAX_SONGS];
static song_settings_cache_t s_settings[SONGLIST_MAX_SONGS];
static uint8_t      s_song_count = 0;
static int16_t      s_focused_idx = 0;  /* currently focused list-button index */
static char         s_playlist_names[SONGLIST_MAX_PLAYLISTS][SONGLIST_PLAYLIST_NAME_LEN];
static uint8_t      s_playlist_count = 0;
static char         s_active_playlist[SONGLIST_PLAYLIST_NAME_LEN] = {0};

/* LVGL objects */
static lv_obj_t   *s_screen      = NULL;
static lv_obj_t   *s_list        = NULL;
static lv_obj_t   *s_playlist_title_label = NULL;
static lv_obj_t   *s_playlist_overlay = NULL;
static lv_group_t *s_group       = NULL;
static lv_indev_t *s_enc_indev   = NULL; /* virtual encoder indev */

/* WiFi toggle button */
static lv_obj_t  *s_wifi_btn     = NULL;   /* clickable container */
static lv_obj_t  *s_wifi_icon    = NULL;   /* label: LV_SYMBOL_WIFI */
static lv_obj_t  *s_wifi_slash   = NULL;   /* "\/" overlay when disabled */
static bool       s_wifi_enabled = false;  /* starts disabled */

/* BT enable/disable button */
static lv_obj_t *s_bt_btn     = NULL;
static lv_obj_t *s_bt_icon    = NULL;
static bool      s_bt_enabled = false; /* starts disabled */

/* Settings dialog – at most one open at a time */
static lv_obj_t  *s_settings_overlay   = NULL; /* backdrop (NULL when not open) */
static uint16_t   s_settings_song_id   = 0;    /* song_id whose dialog is open  */
static lv_obj_t  *s_dd_end_action      = NULL; /* 0=none, 1=next, 2=loop */
static lv_obj_t  *s_dd_downmix         = NULL; /* 0=mix, 1=ch1, 2=ch2 */
static uint8_t    s_end_action         = 0u;
static uint8_t    s_downmix_mode       = 0u;
static lv_obj_t  *s_cb_fixed_speed     = NULL;
static lv_obj_t  *s_btn_speed_minus     = NULL;
static lv_obj_t  *s_btn_speed_plus      = NULL;
static lv_obj_t  *s_cb_light_organ     = NULL; /* light-organ mode checkbox        */
static lv_obj_t  *s_lbl_pitch_val      = NULL;
static uint8_t    s_pitch_influence    = 0u;   /* dialog pitch influence 0-100   */
static lv_obj_t  *s_lbl_speed_val      = NULL; /* speed value label               */
static uint8_t    s_speed_x100         = 100;  /* dialog speed × 100 (70–140)    */
static lv_obj_t  *s_lbl_dmax_val       = NULL;
static lv_obj_t  *s_lbl_dmin_val       = NULL;
static lv_obj_t  *s_lbl_drps_val       = NULL;
static lv_obj_t  *s_lbl_dhld_val       = NULL;
static lv_obj_t  *s_lbl_dfad_val       = NULL;  /* fade-in duration label */
static lv_obj_t  *s_lbl_dmxfade_val    = NULL;  /* downmix fade duration label */
static uint8_t    s_dimmer_max         = 100u;
static uint8_t    s_dimmer_min         = 0u;
static uint8_t    s_dimmer_rps_x10     = 14u; /* 1.4 rps default */
static uint8_t    s_dimmer_holdoff_s   = 0u;
static uint8_t    s_dimmer_fadein_s    = 0u;
static uint8_t    s_downmix_fade_s     = 1u;
/* ---------- Forward declarations ----------------------------------------- */
static void on_list_item_clicked(lv_event_t *e);
static void on_playlist_btn_clicked(lv_event_t *e);
static void on_playlist_pick_clicked(lv_event_t *e);
static void on_playlist_close_clicked(lv_event_t *e);
static void on_wifi_btn_clicked(lv_event_t *e);
static void update_wifi_btn_style(void);
static void on_bt_btn_clicked(lv_event_t *e);
static void update_bt_btn_style(void);
static void update_playlist_caption(void);
static void update_dimmer_labels(void);
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
    uint8_t  dimmer_fadein_s;
    uint8_t  pitch_influence_pct;
    uint8_t  downmix_mode;
    uint8_t  downmix_fade_s;
} async_song_settings_t;

typedef struct {
    int8_t steps;
} async_encoder_move_t;

typedef struct {
    uint8_t count;
    char active[SONGLIST_PLAYLIST_NAME_LEN];
    char names[SONGLIST_MAX_PLAYLISTS][SONGLIST_PLAYLIST_NAME_LEN];
} async_playlists_t;

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
 * Playlist selector
 * ========================================================================= */

static void update_playlist_caption(void)
{
    if (!s_playlist_title_label) return;
    const char *nm = s_active_playlist[0] ? s_active_playlist : "All songs";
    lv_label_set_text(s_playlist_title_label, nm);
}

static void on_playlist_pick_clicked(lv_event_t *e)
{
    uintptr_t pick = (uintptr_t)lv_event_get_user_data(e);
    if (pick == 0u) {
        uart_comm_send_set_active_playlist("");
    } else {
        uint8_t idx = (uint8_t)(pick - 1u);
        if (idx < s_playlist_count) {
            uart_comm_send_set_active_playlist(s_playlist_names[idx]);
        }
    }
    if (s_playlist_overlay) {
        lv_obj_delete(s_playlist_overlay);
        s_playlist_overlay = NULL;
    }
}

static void on_playlist_close_clicked(lv_event_t *e)
{
    (void)e;
    if (s_playlist_overlay) {
        lv_obj_delete(s_playlist_overlay);
        s_playlist_overlay = NULL;
    }
}

static void on_playlist_btn_clicked(lv_event_t *e)
{
    (void)e;
    if (s_playlist_overlay) {
        lv_obj_delete(s_playlist_overlay);
        s_playlist_overlay = NULL;
        return;
    }

    s_playlist_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_playlist_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_playlist_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_playlist_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_playlist_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_playlist_overlay, 0, 0);
    lv_obj_clear_flag(s_playlist_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_playlist_overlay);
    lv_obj_set_size(box, 460, 360);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Select Playlist");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *lst = lv_list_create(box);
    lv_obj_set_size(lst, 432, 250);
    lv_obj_align(lst, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_color(lst, lv_color_hex(0x10223A), 0);
    lv_obj_set_style_bg_opa(lst, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lst, 0, 0);

    lv_obj_t *btn_all = lv_list_add_button(lst, LV_SYMBOL_LIST, "All songs");
    lv_obj_add_event_cb(btn_all, on_playlist_pick_clicked, LV_EVENT_CLICKED, (void *)0u);

    for (uint8_t i = 0; i < s_playlist_count; ++i) {
        char txt[SONGLIST_PLAYLIST_NAME_LEN + 6];
        if (strcmp(s_playlist_names[i], s_active_playlist) == 0) {
            snprintf(txt, sizeof(txt), "%s *", s_playlist_names[i]);
        } else {
            snprintf(txt, sizeof(txt), "%s", s_playlist_names[i]);
        }
        lv_obj_t *b = lv_list_add_button(lst, LV_SYMBOL_AUDIO, txt);
        lv_obj_add_event_cb(b, on_playlist_pick_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)(i + 1u));
    }

    lv_obj_t *close_btn = lv_obj_create(box);
    lv_obj_set_size(close_btn, 140, 42);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2A2A3E), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(0x505060), 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_clear_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(close_btn, on_playlist_close_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xC8C8E0), 0);
    lv_obj_center(cl);
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

    /* Header title: active playlist name, tap to open playlist picker */
    lv_obj_t *title_btn = lv_obj_create(s_screen);
    lv_obj_set_size(title_btn, 400, 44);
    lv_obj_align(title_btn, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(title_btn, lv_color_hex(0x2A2A3E), 0);
    lv_obj_set_style_bg_opa(title_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(title_btn, lv_color_hex(0x1E88E5), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(title_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(title_btn, lv_color_hex(0x505060), 0);
    lv_obj_set_style_border_width(title_btn, 1, 0);
    lv_obj_set_style_radius(title_btn, 10, 0);
    lv_obj_set_style_pad_left(title_btn, 12, 0);
    lv_obj_set_style_pad_right(title_btn, 12, 0);
    lv_obj_set_style_pad_top(title_btn, 0, 0);
    lv_obj_set_style_pad_bottom(title_btn, 0, 0);
    lv_obj_clear_flag(title_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(title_btn, on_playlist_btn_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(title_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_playlist_title_label = lv_label_create(title_btn);
    lv_label_set_text(s_playlist_title_label, "All songs");
    lv_obj_set_style_text_font(s_playlist_title_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_playlist_title_label, lv_color_hex(0xE0E0FF), 0);
    lv_label_set_long_mode(s_playlist_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_playlist_title_label, 340);

    lv_obj_t *title_drop_icon = lv_label_create(title_btn);
    lv_label_set_text(title_drop_icon, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(title_drop_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_drop_icon, lv_color_hex(0xC8C8E0), 0);

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
    lv_coord_t list_h = (lv_coord_t)lv_disp_get_ver_res(lv_disp_get_default()) - 74;
    lv_obj_set_size(s_list, LV_PCT(100), list_h);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 74);
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

        /* Center the selected entry in the visible area while clamping at the
         * list boundaries.  This keeps the external index selection in the middle
         * of the list, and only lets it drift to the edge when the end of the
         * list is reached.
         */
        lv_coord_t item_y = lv_obj_get_y(target);
        lv_coord_t item_h = lv_obj_get_height(target);
        lv_coord_t list_h = lv_obj_get_height(s_list);

        lv_coord_t max_scroll = 0;
        uint16_t child_cnt = lv_obj_get_child_cnt(s_list);
        if (child_cnt > 0) {
            lv_obj_t *last = lv_obj_get_child(s_list, child_cnt - 1u);
            if (last) {
                lv_coord_t last_y = lv_obj_get_y(last) + lv_obj_get_height(last);
                if (last_y > list_h) max_scroll = last_y - list_h;
            }
        }

        lv_coord_t desired_y = item_y - ((list_h - item_h) / 2);

        if (desired_y < 0) desired_y = 0;
        if (desired_y > max_scroll) desired_y = max_scroll;

        lv_obj_scroll_to_y(s_list, desired_y, LV_ANIM_OFF);
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

static void update_speed_buttons_state(void)
{
    bool enabled = s_cb_fixed_speed && ((lv_obj_get_state(s_cb_fixed_speed) & LV_STATE_CHECKED) != 0);

    if (s_btn_speed_minus) {
        if (enabled) {
            lv_obj_clear_state(s_btn_speed_minus, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_speed_minus, LV_STATE_DISABLED);
        }
    }

    if (s_btn_speed_plus) {
        if (enabled) {
            lv_obj_clear_state(s_btn_speed_plus, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_btn_speed_plus, LV_STATE_DISABLED);
        }
    }
}

static void on_fixed_speed_toggled(lv_event_t *e)
{
    (void)e;
    update_speed_buttons_state();
}

static void on_speed_minus(lv_event_t *e)
{
    (void)e;
    if (s_cb_fixed_speed && ((lv_obj_get_state(s_cb_fixed_speed) & LV_STATE_CHECKED) == 0u)) return;
    if (s_speed_x100 > 70u) s_speed_x100 -= 5u;
    if (s_cb_fixed_speed) lv_obj_add_state(s_cb_fixed_speed, LV_STATE_CHECKED);
    update_speed_label();
}

static void on_speed_plus(lv_event_t *e)
{
    (void)e;
    if (s_cb_fixed_speed && ((lv_obj_get_state(s_cb_fixed_speed) & LV_STATE_CHECKED) == 0u)) return;
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
    if (s_lbl_dfad_val) {
        snprintf(buf, sizeof(buf), "%us", s_dimmer_fadein_s);
        lv_label_set_text(s_lbl_dfad_val, buf);
    }
}

static void on_dmax_minus(lv_event_t *e) { (void)e; if (s_dimmer_max > 0u)   s_dimmer_max   -= 5u; if (s_dimmer_max < s_dimmer_min) s_dimmer_max = s_dimmer_min; update_dimmer_labels(); }
static void on_dmax_plus (lv_event_t *e) { (void)e; if (s_dimmer_max < 100u) s_dimmer_max   += 5u; if (s_dimmer_max > 100u) s_dimmer_max = 100u;             update_dimmer_labels(); }
static void on_dmin_minus(lv_event_t *e) { (void)e; if (s_dimmer_min > 0u)   s_dimmer_min   -= 5u;                                                            update_dimmer_labels(); }
static void on_dmin_plus (lv_event_t *e) { (void)e; if (s_dimmer_min < 100u) s_dimmer_min   += 5u; if (s_dimmer_min > s_dimmer_max) s_dimmer_min = s_dimmer_max; update_dimmer_labels(); }
static void on_drps_minus(lv_event_t *e) { (void)e; if (s_dimmer_rps_x10 > 1u) s_dimmer_rps_x10 -= 1u;                                                        update_dimmer_labels(); }
static void on_drps_plus (lv_event_t *e) { (void)e; if (s_dimmer_rps_x10 < 30u) s_dimmer_rps_x10 += 1u;                                                       update_dimmer_labels(); }
static void on_dhld_minus(lv_event_t *e) { (void)e; if (s_dimmer_holdoff_s > 0u)   s_dimmer_holdoff_s -= 1u; update_dimmer_labels(); }
static void on_dhld_plus (lv_event_t *e) { (void)e; if (s_dimmer_holdoff_s < 255u) s_dimmer_holdoff_s += 1u; update_dimmer_labels(); }
static void on_dfad_minus(lv_event_t *e) { (void)e; if (s_dimmer_fadein_s > 0u)    s_dimmer_fadein_s  -= 1u; update_dimmer_labels(); }
static void on_dfad_plus (lv_event_t *e) { (void)e; if (s_dimmer_fadein_s < 60u)   s_dimmer_fadein_s  += 1u; update_dimmer_labels(); }
static void on_dmxfade_minus(lv_event_t *e)
{
    (void)e;
    if (s_downmix_fade_s > 0u) s_downmix_fade_s -= 1u;
    if (s_lbl_dmxfade_val) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%us", s_downmix_fade_s);
        lv_label_set_text(s_lbl_dmxfade_val, buf);
    }
}
static void on_dmxfade_plus(lv_event_t *e)
{
    (void)e;
    if (s_downmix_fade_s < 10u) s_downmix_fade_s += 1u;
    if (s_lbl_dmxfade_val) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%us", s_downmix_fade_s);
        lv_label_set_text(s_lbl_dmxfade_val, buf);
    }
}

static void on_settings_cancel(lv_event_t *e)
{
    (void)e;
    if (s_settings_overlay) {
        lv_obj_delete(s_settings_overlay);
        s_settings_overlay = NULL;
        s_settings_song_id = 0;
        s_dd_end_action    = NULL;
        s_dd_downmix       = NULL;
        s_cb_fixed_speed   = NULL;
        s_btn_speed_minus  = NULL;
        s_btn_speed_plus   = NULL;
        s_cb_light_organ   = NULL;
        s_lbl_pitch_val    = NULL;
        s_lbl_dmax_val     = NULL;
        s_lbl_dmin_val     = NULL;
        s_lbl_drps_val     = NULL;
        s_lbl_dhld_val     = NULL;
        s_lbl_dfad_val     = NULL;
        s_lbl_dmxfade_val  = NULL;
    }
}

static void on_settings_ok(lv_event_t *e)
{
    (void)e;
    if (!s_settings_overlay) return;

    uint16_t song_id = s_settings_song_id;

    /* Collect checkbox states */
    if (s_dd_end_action) {
        s_end_action = (uint8_t)lv_dropdown_get_selected(s_dd_end_action);
        if (s_end_action > 2u) s_end_action = 0u;
    } else {
        s_end_action = 0u;
    }
    if (s_dd_downmix) {
        s_downmix_mode = (uint8_t)lv_dropdown_get_selected(s_dd_downmix);
        if (s_downmix_mode > 2u) s_downmix_mode = 0u;
    } else {
        s_downmix_mode = 0u;
    }
    bool speed_en      = (lv_obj_get_state(s_cb_fixed_speed) & LV_STATE_CHECKED) != 0;
    bool light_org_en  = s_cb_light_organ
                         ? ((lv_obj_get_state(s_cb_light_organ) & LV_STATE_CHECKED) != 0)
                         : false;

    uint8_t flags = 0;
    if (s_end_action == 2u) flags |= 0x01u; /* loop */
    if (s_end_action == 1u) flags |= 0x04u; /* autoplay next */
    if (speed_en)      flags |= 0x02u;
    if (light_org_en)  flags |= 0x10u;
    uint8_t fixed_speed_x100 = speed_en ? s_speed_x100 : 100u;
    uint8_t d_max  = s_dimmer_max;
    uint8_t d_min  = s_dimmer_min;
    uint8_t d_rps  = s_dimmer_rps_x10;
    uint8_t d_hld  = s_dimmer_holdoff_s;
    uint8_t d_fad  = s_dimmer_fadein_s;

    /* Update local cache */
    uint8_t cache_idx = (uint8_t)(song_id - 1);
    if (cache_idx < SONGLIST_MAX_SONGS) {
        s_settings[cache_idx].flags               = flags;
        s_settings[cache_idx].fixed_speed_x100    = fixed_speed_x100;
        s_settings[cache_idx].dimmer_max          = d_max;
        s_settings[cache_idx].dimmer_min          = d_min;
        s_settings[cache_idx].dimmer_rps_ref_x10  = d_rps;
        s_settings[cache_idx].dimmer_holdoff_s    = d_hld;
        s_settings[cache_idx].dimmer_fadein_s     = d_fad;
        s_settings[cache_idx].pitch_influence_pct = s_pitch_influence;
        s_settings[cache_idx].downmix_mode        = s_downmix_mode;
        s_settings[cache_idx].downmix_fade_s      = s_downmix_fade_s;
        s_settings[cache_idx].valid               = true;
    }

    /* Send to player and request fresh response so other views update */
    uart_comm_send_set_song_settings(song_id, flags, fixed_speed_x100,
                                     d_max, d_min, d_rps, d_hld, d_fad, s_pitch_influence,
                                     s_downmix_mode, s_downmix_fade_s);
    uart_comm_send_song_settings_req(song_id);
    ESP_LOGI(TAG, "Settings saved: song_id=%u flags=0x%02X", song_id, flags);

    /* Close dialog */
    lv_obj_delete(s_settings_overlay);
    s_settings_overlay   = NULL;
    s_settings_song_id   = 0;
    s_dd_end_action      = NULL;
    s_dd_downmix         = NULL;
    s_cb_fixed_speed     = NULL;
    s_btn_speed_minus    = NULL;
    s_btn_speed_plus     = NULL;
    s_cb_light_organ     = NULL;
    s_lbl_pitch_val      = NULL;
    s_lbl_speed_val      = NULL;
    s_lbl_dmax_val       = NULL;
    s_lbl_dmin_val       = NULL;
    s_lbl_drps_val       = NULL;
    s_lbl_dhld_val       = NULL;
    s_lbl_dfad_val       = NULL;
    s_lbl_dmxfade_val    = NULL;
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

    /* Determine current values from cache (factory defaults if not yet saved). */
    uint8_t cache_idx = (uint8_t)(song_id - 1);
    uint8_t cached_end_action      = 0u; /* 0=none, 1=next, 2=loop */
    bool    cached_speed           = false;
    bool    cached_light_organ     = false;
    uint8_t cached_pitch_influence = 0u;
    uint8_t cached_spd_x100        = 100u;
    uint8_t cached_dmax            = 100u;
    uint8_t cached_dmin            = 0u;
    uint8_t cached_drps            = 14u;
    uint8_t cached_dhld            = 0u;
    uint8_t cached_dfad            = 0u;
    uint8_t cached_downmix_mode    = 0u;
    uint8_t cached_downmix_fade_s  = 1u;
    if (cache_idx < SONGLIST_MAX_SONGS && s_settings[cache_idx].valid) {
        bool cached_loop          = (s_settings[cache_idx].flags & 0x01u) != 0;
        bool cached_autoplay_next = (s_settings[cache_idx].flags & 0x04u) != 0;
        if (cached_loop) cached_end_action = 2u;
        else if (cached_autoplay_next) cached_end_action = 1u;
        cached_speed           = (s_settings[cache_idx].flags & 0x02u) != 0;
        cached_light_organ     = (s_settings[cache_idx].flags & 0x10u) != 0;
        cached_pitch_influence = s_settings[cache_idx].pitch_influence_pct;
        cached_spd_x100        = s_settings[cache_idx].fixed_speed_x100;
        if (cached_spd_x100 < 70u || cached_spd_x100 > 140u) cached_spd_x100 = 100u;
        cached_dmax = s_settings[cache_idx].dimmer_max;
        cached_dmin = s_settings[cache_idx].dimmer_min;
        cached_drps = s_settings[cache_idx].dimmer_rps_ref_x10;
        if (cached_drps == 0u) cached_drps = 14u;
        cached_dhld = s_settings[cache_idx].dimmer_holdoff_s;
        cached_dfad = s_settings[cache_idx].dimmer_fadein_s;
        cached_downmix_mode = s_settings[cache_idx].downmix_mode;
        if (cached_downmix_mode > 2u) cached_downmix_mode = 0u;
        cached_downmix_fade_s = s_settings[cache_idx].downmix_fade_s;
        if (cached_downmix_fade_s > 10u) cached_downmix_fade_s = 1u;
    }
    s_pitch_influence  = cached_pitch_influence;
    s_end_action       = cached_end_action;
    s_speed_x100       = cached_spd_x100;
    s_dimmer_max       = cached_dmax;
    s_dimmer_min       = cached_dmin;
    s_dimmer_rps_x10   = cached_drps;
    s_dimmer_holdoff_s = cached_dhld;
    s_dimmer_fadein_s  = cached_dfad;
    s_downmix_mode     = cached_downmix_mode;
    s_downmix_fade_s   = cached_downmix_fade_s;

    s_settings_song_id = song_id;

    /* ── Backdrop ─────────────────────────────────────────────────── */
    s_settings_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_settings_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_settings_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_settings_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_settings_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_settings_overlay, 0, 0);
    lv_obj_clear_flag(s_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Backdrop blocks the songlist; only OK / Cancel buttons dismiss the dialog. */

    /* ── Dialog box ───────────────────────────────────────────────── */
    lv_obj_t *box = lv_obj_create(s_settings_overlay);
    lv_obj_set_size(box, 520, 452);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

    /* ── Title ────────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text_fmt(title, LV_SYMBOL_SETTINGS "  %s", song_name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1E88E5), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, 452);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    /* ── Scrollable content area (all settings in one vertical column) ── */
    /* Leave a footer strip for Cancel/OK so lower rows aren't hidden underneath. */
    lv_obj_t *content = lv_obj_create(box);
    lv_obj_set_size(content, 472, 310);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 24, 54);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ACTIVE);

    /* ── Sound controls ───────────────────────────────────────────── */
    {
        lv_obj_t *ea_lbl = lv_label_create(content);
        lv_label_set_text(ea_lbl, "When song ends");
        lv_obj_set_style_text_font(ea_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ea_lbl, lv_color_hex(0xA0A0C0), 0);
        lv_obj_align(ea_lbl, LV_ALIGN_TOP_LEFT, 0, 8);

        s_dd_end_action = lv_dropdown_create(content);
        lv_dropdown_set_options(s_dd_end_action,
                                "Stop (default)\n"
                                "Play next track\n"
                                "Repeat this track");
        lv_dropdown_set_selected(s_dd_end_action, s_end_action);
        lv_obj_set_style_text_font(s_dd_end_action, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_dd_end_action, lv_color_hex(0xE0E0FF), 0);
        lv_obj_set_style_bg_color(s_dd_end_action, lv_color_hex(0x10223A), 0);
        lv_obj_set_style_border_color(s_dd_end_action, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_border_width(s_dd_end_action, 1, 0);
        lv_obj_set_style_radius(s_dd_end_action, 6, 0);
        lv_obj_set_width(s_dd_end_action, 340);
        lv_obj_align(s_dd_end_action, LV_ALIGN_TOP_LEFT, 0, 36);
    }

    {
        lv_obj_t *dmx_lbl = lv_label_create(content);
        lv_label_set_text(dmx_lbl, "Default downmix");
        lv_obj_set_style_text_font(dmx_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(dmx_lbl, lv_color_hex(0xA0A0C0), 0);
        lv_obj_align(dmx_lbl, LV_ALIGN_TOP_LEFT, 0, 88);

        s_dd_downmix = lv_dropdown_create(content);
        lv_dropdown_set_options(s_dd_downmix,
                                "Mix CH1+CH2\n"
                                "CH1 only\n"
                                "CH2 only");
        lv_dropdown_set_selected(s_dd_downmix, s_downmix_mode);
        lv_obj_set_style_text_font(s_dd_downmix, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_dd_downmix, lv_color_hex(0xE0E0FF), 0);
        lv_obj_set_style_bg_color(s_dd_downmix, lv_color_hex(0x10223A), 0);
        lv_obj_set_style_border_color(s_dd_downmix, lv_color_hex(0x1E88E5), 0);
        lv_obj_set_style_border_width(s_dd_downmix, 1, 0);
        lv_obj_set_style_radius(s_dd_downmix, 6, 0);
        lv_obj_set_width(s_dd_downmix, 340);
        lv_obj_align(s_dd_downmix, LV_ALIGN_TOP_LEFT, 0, 116);
    }

    /* Reuse MAKE_DIMMER_ROW for speed and pitch spinners */
#define MAKE_DIMMER_ROW(par, ypos, lbl_txt, on_m, on_p, lbl_ref)                     \
    do {                                                                               \
        lv_obj_t *_rl = lv_label_create(par);                                         \
        lv_label_set_text(_rl, lbl_txt);                                               \
        lv_obj_set_style_text_font(_rl, &lv_font_montserrat_20, 0);                   \
        lv_obj_set_style_text_color(_rl, lv_color_hex(0xA0A0C0), 0);                  \
        lv_obj_set_width(_rl, 170);                                                    \
        lv_obj_align(_rl, LV_ALIGN_TOP_LEFT, 0, (ypos) + 8);                            \
        lv_obj_t *_rm = lv_button_create(par);                                         \
        lv_obj_set_size(_rm, 48, 48);                                                  \
        lv_obj_align(_rm, LV_ALIGN_TOP_LEFT, 208, (ypos));                             \
        lv_obj_set_style_bg_color(_rm, lv_color_hex(0x1E88E5), 0);                    \
        lv_obj_set_style_bg_color(_rm, lv_color_hex(0x1565C0), LV_STATE_PRESSED);     \
        lv_obj_set_style_radius(_rm, 8, 0);                                            \
        lv_obj_set_style_border_width(_rm, 0, 0);                                      \
        lv_obj_add_event_cb(_rm, on_m, LV_EVENT_CLICKED, NULL);                        \
        lv_obj_t *_rml = lv_label_create(_rm);                                         \
        lv_label_set_text(_rml, "-");                                                 \
        lv_obj_set_style_text_font(_rml, &lv_font_montserrat_28, 0);                   \
        lv_obj_set_style_text_color(_rml, lv_color_white(), 0);                        \
        lv_obj_center(_rml);                                                            \
        (lbl_ref) = lv_label_create(par);                                              \
        lv_obj_set_size((lbl_ref), 90, 40);                                            \
        lv_obj_align((lbl_ref), LV_ALIGN_TOP_LEFT, 268, (ypos) + 4);                   \
        lv_obj_set_style_text_font((lbl_ref), &lv_font_montserrat_20, 0);               \
        lv_obj_set_style_text_color((lbl_ref), lv_color_hex(0xE0E0FF), 0);              \
        lv_obj_set_style_text_align((lbl_ref), LV_TEXT_ALIGN_CENTER, 0);                \
        lv_obj_t *_rp = lv_button_create(par);                                         \
        lv_obj_set_size(_rp, 48, 48);                                                  \
        lv_obj_align(_rp, LV_ALIGN_TOP_LEFT, 372, (ypos));                             \
        lv_obj_set_style_bg_color(_rp, lv_color_hex(0x1E88E5), 0);                    \
        lv_obj_set_style_bg_color(_rp, lv_color_hex(0x1565C0), LV_STATE_PRESSED);     \
        lv_obj_set_style_radius(_rp, 8, 0);                                            \
        lv_obj_set_style_border_width(_rp, 0, 0);                                      \
        lv_obj_add_event_cb(_rp, on_p, LV_EVENT_CLICKED, NULL);                        \
        lv_obj_t *_rpl = lv_label_create(_rp);                                         \
        lv_label_set_text(_rpl, "+");                                                 \
        lv_obj_set_style_text_font(_rpl, &lv_font_montserrat_28, 0);                   \
        lv_obj_set_style_text_color(_rpl, lv_color_white(), 0);                        \
        lv_obj_center(_rpl);                                                            \
    } while(0)

    MAKE_DIMMER_ROW(content, 185, "Downmix fade", on_dmxfade_minus, on_dmxfade_plus, s_lbl_dmxfade_val);
    if (s_lbl_dmxfade_val) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%us", s_downmix_fade_s);
        lv_label_set_text(s_lbl_dmxfade_val, buf);
    }

    s_cb_fixed_speed = lv_checkbox_create(content);
    lv_checkbox_set_text(s_cb_fixed_speed, "Fixed Speed");
    if (cached_speed) lv_obj_add_state(s_cb_fixed_speed, LV_STATE_CHECKED);
    lv_obj_set_style_text_font(s_cb_fixed_speed, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cb_fixed_speed, lv_color_hex(0xE0E0FF), 0);
    lv_obj_align(s_cb_fixed_speed, LV_ALIGN_TOP_LEFT, 0, 255);
    lv_obj_add_event_cb(s_cb_fixed_speed, on_fixed_speed_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    s_btn_speed_minus = lv_button_create(content);
    lv_obj_set_size(s_btn_speed_minus, 48, 48);
    lv_obj_align(s_btn_speed_minus, LV_ALIGN_TOP_LEFT, 208, 245);
    lv_obj_set_style_bg_color(s_btn_speed_minus, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_bg_color(s_btn_speed_minus, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_btn_speed_minus, 8, 0);
    lv_obj_set_style_border_width(s_btn_speed_minus, 0, 0);
    lv_obj_add_event_cb(s_btn_speed_minus, on_speed_minus, LV_EVENT_CLICKED, NULL);
    lv_obj_t *speed_minus_label = lv_label_create(s_btn_speed_minus);
    lv_label_set_text(speed_minus_label, "-");
    lv_obj_set_style_text_font(speed_minus_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(speed_minus_label, lv_color_white(), 0);
    lv_obj_center(speed_minus_label);

    s_lbl_speed_val = lv_label_create(content);
    lv_obj_set_size(s_lbl_speed_val, 90, 40);
    lv_obj_align(s_lbl_speed_val, LV_ALIGN_TOP_LEFT, 268, 249);
    lv_obj_set_style_text_font(s_lbl_speed_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_speed_val, lv_color_hex(0xE0E0FF), 0);
    lv_obj_set_style_text_align(s_lbl_speed_val, LV_TEXT_ALIGN_CENTER, 0);
    update_speed_label();

    s_btn_speed_plus = lv_button_create(content);
    lv_obj_set_size(s_btn_speed_plus, 48, 48);
    lv_obj_align(s_btn_speed_plus, LV_ALIGN_TOP_LEFT, 372, 245);
    lv_obj_set_style_bg_color(s_btn_speed_plus, lv_color_hex(0x1E88E5), 0);
    lv_obj_set_style_bg_color(s_btn_speed_plus, lv_color_hex(0x1565C0), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_btn_speed_plus, 8, 0);
    lv_obj_set_style_border_width(s_btn_speed_plus, 0, 0);
    lv_obj_add_event_cb(s_btn_speed_plus, on_speed_plus, LV_EVENT_CLICKED, NULL);
    lv_obj_t *speed_plus_label = lv_label_create(s_btn_speed_plus);
    lv_label_set_text(speed_plus_label, "+");
    lv_obj_set_style_text_font(speed_plus_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(speed_plus_label, lv_color_white(), 0);
    lv_obj_center(speed_plus_label);
    update_speed_buttons_state();

    MAKE_DIMMER_ROW(content, 310, "Pitch %", on_pitch_minus, on_pitch_plus, s_lbl_pitch_val);
    update_pitch_label();

    /* ── Separator ────────────────────────────────────────────────── */
    {
        lv_obj_t *sep = lv_obj_create(content);
        lv_obj_set_size(sep, 452, 1);
        lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, 392);
        lv_obj_set_style_bg_color(sep, lv_color_hex(0x2A3A5A), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_pad_all(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *dim_lbl = lv_label_create(content);
        lv_label_set_text(dim_lbl, "Dimmer");
        lv_obj_set_style_text_font(dim_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(dim_lbl, lv_color_hex(0x6d6d8a), 0);
        lv_obj_align(dim_lbl, LV_ALIGN_TOP_LEFT, 0, 410);
    }

    /* ── Dimmer controls ──────────────────────────────────────────── */
    s_cb_light_organ = lv_checkbox_create(content);
    lv_checkbox_set_text(s_cb_light_organ, "Light organ (FFT)");
    if (cached_light_organ) lv_obj_add_state(s_cb_light_organ, LV_STATE_CHECKED);
    lv_obj_set_style_text_font(s_cb_light_organ, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cb_light_organ, lv_color_hex(0xE0E0FF), 0);
    lv_obj_align(s_cb_light_organ, LV_ALIGN_TOP_LEFT, 0, 440);

    MAKE_DIMMER_ROW(content, 470, "Max bright",  on_dmax_minus, on_dmax_plus, s_lbl_dmax_val);
    MAKE_DIMMER_ROW(content, 530, "Min bright",  on_dmin_minus, on_dmin_plus, s_lbl_dmin_val);
    MAKE_DIMMER_ROW(content, 590, "Full at RPS", on_drps_minus, on_drps_plus, s_lbl_drps_val);

    /* ── Separator ────────────────────────────────────────────────── */
    {
        lv_obj_t *sep2 = lv_obj_create(content);
        lv_obj_set_size(sep2, 452, 1);
        lv_obj_align(sep2, LV_ALIGN_TOP_LEFT, 0, 665);
        lv_obj_set_style_bg_color(sep2, lv_color_hex(0x2A3A5A), 0);
        lv_obj_set_style_bg_opa(sep2, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep2, 0, 0);
        lv_obj_set_style_pad_all(sep2, 0, 0);
        lv_obj_clear_flag(sep2, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *hld_lbl = lv_label_create(content);
        lv_label_set_text(hld_lbl, "Lamp timing");
        lv_obj_set_style_text_font(hld_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(hld_lbl, lv_color_hex(0x6d6d8a), 0);
        lv_obj_align(hld_lbl, LV_ALIGN_TOP_LEFT, 0, 684);
    }

    MAKE_DIMMER_ROW(content, 710, "Holdoff",  on_dhld_minus, on_dhld_plus, s_lbl_dhld_val);
    MAKE_DIMMER_ROW(content, 770, "Fade-in",  on_dfad_minus, on_dfad_plus, s_lbl_dfad_val);
    update_dimmer_labels();

#undef MAKE_DIMMER_ROW

    /* ── Cancel / OK ─────────────────────────────────────────────── */
    lv_obj_t *btn_cancel = lv_obj_create(box);
    lv_obj_set_size(btn_cancel, 180, 44);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 18, -14);
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
    lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_RIGHT, -18, -14);
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
            uint8_t next = (i + 1) % s_song_count;
            return s_songs[next].id;
        }
    }
    return s_songs[0].id;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

void ui_songlist_show(void)
{
    lv_screen_load(s_screen);
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
 * Song-list update (UART task → LVGL task)
 * ========================================================================= */

static void async_cb_update_songlist(void *user_data)
{
    async_songlist_payload_t *p = (async_songlist_payload_t *)user_data;

    s_song_count = 0;
    memset(s_settings, 0, sizeof(s_settings));

    const uint8_t *ptr = p->data;
    const uint8_t *end = p->data + p->len;

    while (ptr + 2 < end) {
        uint16_t id = (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
        ptr += 2;
        if (id == 0) break; /* end-of-list sentinel */

        const uint8_t *name_start = ptr;
        while (ptr < end && *ptr != '\0') ptr++;
        if (ptr >= end) break;
        ptr++; /* skip '\0' */

        if (s_song_count < SONGLIST_MAX_SONGS) {
            s_songs[s_song_count].id = id;
            size_t name_len = (size_t)(ptr - name_start - 1);
            if (name_len >= MAX_SONG_NAME_LEN) name_len = MAX_SONG_NAME_LEN - 1;
            memcpy(s_songs[s_song_count].name, name_start, name_len);
            s_songs[s_song_count].name[name_len] = '\0';
            s_song_count++;
        }
    }

    free(p->data);
    free(p);

    rebuild_list();
    ESP_LOGI(TAG, "Song list updated: %u songs", s_song_count);
}

void ui_songlist_update_async(const uint8_t *data, uint16_t len)
{
    if (!s_screen || !data || len == 0) return;

    async_songlist_payload_t *p = malloc(sizeof(async_songlist_payload_t));
    if (!p) { ESP_LOGE(TAG, "OOM in update_async"); return; }

    p->data = malloc(len);
    if (!p->data) { free(p); ESP_LOGE(TAG, "OOM in update_async data"); return; }

    memcpy(p->data, data, len);
    p->len = len;

    lv_lock();
    lv_async_call(async_cb_update_songlist, p);
    lv_unlock();
}

/* =========================================================================
 * Encoder async bridges
 * ========================================================================= */

static void async_cb_encoder_move(void *user_data)
{
    async_encoder_move_t *p = (async_encoder_move_t *)user_data;
    focus_item(s_focused_idx + p->steps);
    free(p);
}

void ui_songlist_encoder_move_async(int8_t steps)
{
    if (!s_screen) return;
    async_encoder_move_t *p = malloc(sizeof(async_encoder_move_t));
    if (!p) return;
    p->steps = steps;
    lv_lock();
    lv_async_call(async_cb_encoder_move, p);
    lv_unlock();
}

static void async_cb_encoder_btn(void *user_data)
{
    (void)user_data;
    if (s_focused_idx >= 0 && s_focused_idx < (int16_t)s_song_count) {
        send_play_song(s_songs[s_focused_idx].id);
    }
}

void ui_songlist_encoder_btn_async(void)
{
    if (!s_screen) return;
    lv_lock();
    lv_async_call(async_cb_encoder_btn, NULL);
    lv_unlock();
}

/* =========================================================================
 * Song-settings delivery (UART task → LVGL task)
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
        s_settings[idx].dimmer_fadein_s     = p->dimmer_fadein_s;
        s_settings[idx].pitch_influence_pct = p->pitch_influence_pct;
        s_settings[idx].downmix_mode        = p->downmix_mode;
        s_settings[idx].downmix_fade_s      = p->downmix_fade_s;
        s_settings[idx].valid               = true;
    }

    /* Cache-only update. Never push to the live dialog –
     * that would race with the user's in-progress edits. */

    free(p);
}

void ui_songlist_song_settings_async(uint16_t song_id, uint8_t flags, uint8_t fixed_speed_x100,
                                     uint8_t dimmer_max, uint8_t dimmer_min,
                                     uint8_t dimmer_rps_ref_x10, uint8_t dimmer_holdoff_s,
                                     uint8_t dimmer_fadein_s, uint8_t pitch_influence_pct,
                                     uint8_t downmix_mode,
                                     uint8_t downmix_fade_s)
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
    p->dimmer_fadein_s      = dimmer_fadein_s;
    p->pitch_influence_pct  = pitch_influence_pct;
    p->downmix_mode         = (downmix_mode <= 2u) ? downmix_mode : 0u;
    p->downmix_fade_s       = (downmix_fade_s <= 10u) ? downmix_fade_s : 1u;

    lv_lock();
    lv_async_call(async_cb_song_settings, p);
    lv_unlock();
}

/* =========================================================================
 * BT / WiFi state sync
 * ========================================================================= */

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

static void async_cb_update_wifi_enabled(void *user_data)
{
    bool enabled = (bool)(uintptr_t)user_data;
    s_wifi_enabled = enabled;
    if (s_wifi_btn) update_wifi_btn_style();
}

static void async_cb_playlists(void *user_data)
{
    async_playlists_t *p = (async_playlists_t *)user_data;
    s_playlist_count = (p->count <= SONGLIST_MAX_PLAYLISTS) ? p->count : SONGLIST_MAX_PLAYLISTS;
    strncpy(s_active_playlist, p->active, SONGLIST_PLAYLIST_NAME_LEN - 1);
    s_active_playlist[SONGLIST_PLAYLIST_NAME_LEN - 1] = '\0';
    for (uint8_t i = 0; i < s_playlist_count; ++i) {
        strncpy(s_playlist_names[i], p->names[i], SONGLIST_PLAYLIST_NAME_LEN - 1);
        s_playlist_names[i][SONGLIST_PLAYLIST_NAME_LEN - 1] = '\0';
    }
    update_playlist_caption();
    free(p);
}

void ui_songlist_playlists_async(const char *active_name,
                                 const char names[][SONGLIST_PLAYLIST_NAME_LEN],
                                 uint8_t count)
{
    if (!s_screen) return;
    async_playlists_t *p = (async_playlists_t *)malloc(sizeof(async_playlists_t));
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->count = (count <= SONGLIST_MAX_PLAYLISTS) ? count : SONGLIST_MAX_PLAYLISTS;
    if (active_name) {
        strncpy(p->active, active_name, SONGLIST_PLAYLIST_NAME_LEN - 1);
        p->active[SONGLIST_PLAYLIST_NAME_LEN - 1] = '\0';
    }
    for (uint8_t i = 0; i < p->count; ++i) {
        strncpy(p->names[i], names[i], SONGLIST_PLAYLIST_NAME_LEN - 1);
        p->names[i][SONGLIST_PLAYLIST_NAME_LEN - 1] = '\0';
    }
    lv_lock();
    lv_async_call(async_cb_playlists, p);
    lv_unlock();
}

void ui_songlist_update_wifi_enabled_async(bool enabled)
{
    if (!s_screen) return;
    lv_lock();
    lv_async_call(async_cb_update_wifi_enabled, (void *)(uintptr_t)enabled);
    lv_unlock();
}

/* =========================================================================
 * send_play_song (internal helper used by list-item click and encoder btn)
 * ========================================================================= */

static void send_play_song(uint16_t song_id)
{
    ESP_LOGI(TAG, "send_play_song: id=%u", song_id);
    uart_comm_send_play_song(song_id);
}
