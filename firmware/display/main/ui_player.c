/**
 * @file ui_player.c
 * @brief Player view – song title, progress animation, live poti bars, STOP.
 *
 * Layout (800×480):
 *   Left panel  (x=0..599,  w=600): title, "NOW PLAYING", progress bar, STOP button
 *   Divider     (x=597..599, w=2):  vertical separator
 *   Right panel (x=600..799, w=200): VOL / TMP vertical bar indicators
 *
 * Thread safety: all *_async() functions post lv_async_call() items to the
 * LVGL task.  The STOP button callback and all non-async functions run inside
 * the LVGL task (lv_timer_handler) and may call LVGL APIs directly.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#include "lvgl.h"
#include "ui_player.h"
#include "ui_songlist.h"
#include "uart_comm.h"

static const char *TAG = "ui_player";

/* =========================================================================
 * Layout constants
 * ========================================================================= */
#define SPLIT_X          600    /* left / right panel boundary              */
#define RIGHT_W          200    /* right panel width                        */
#define SCREEN_H         480

/* Left panel */
#define TITLE_Y          28     /* song title top edge                      */
#define NOWPLAY_Y        82     /* "NOW PLAYING" label top edge             */
#define PROGRESS_Y       130    /* progress bar top edge                    */
#define PROGRESS_H       44     /* progress bar height                      */
#define PROGRESS_PAD_X   30     /* horizontal padding inside left panel     */
#define STOP_Y           370    /* STOP button top edge                     */
#define STOP_W           300    /* STOP button width                        */
#define STOP_H           80     /* STOP button height                       */
#define BYPASS_CHECK_Y_R (VAL_LABEL_Y + 24) /* bypass label / lock checkbox row */
#define TIME_LABEL_Y     (PROGRESS_Y + PROGRESS_H + 8)  /* elapsed/total label */
#define STATUS_LBL_Y     (TIME_LABEL_Y + 22)  /* loop / 1.0x indicator row (left panel) */
#define GEAR_BTN_W       80
#define GEAR_BTN_H       80
/* Gear button x: right of the STOP button with a 12-px gap */
#define GEAR_BTN_X       ((SPLIT_X - STOP_W) / 2 + STOP_W + 12)

/* Right panel – two indicator columns */
#define COL_W            (RIGHT_W / 2)   /* 100 px per column               */
#define BAR_W            34              /* indicator bar width              */
#define BAR_H            300             /* indicator bar height             */
#define BAR_TOP_Y        55              /* bar top edge inside right panel  */
#define COLLABEL_Y       12              /* column name label top edge       */
#define VAL_LABEL_Y      (BAR_TOP_Y + BAR_H + 10) /* value text top edge   */

/* Accent colours */
#define COLOR_BG         0x1A1A2E
#define COLOR_PANEL_R    0x16213E
#define COLOR_ACCENT     0x00B4D8   /* cyan – "now playing", bar indicator  */
#define COLOR_STOP_BG    0xE94560   /* red stop button                      */
#define COLOR_TEXT       0xE0E0FF   /* near-white text                      */
#define COLOR_BAR_TRACK  0x0A2744   /* dark bar background                  */
#define COLOR_DIVIDER    0x2A2A5E
#define COLOR_LOCKED     0xF39C12   /* amber – tempo locked                 */

/* =========================================================================
 * Widget handles
 * ========================================================================= */
static lv_obj_t *s_screen         = NULL;
static lv_obj_t *s_title_lbl      = NULL;
static lv_obj_t *s_progress_bar   = NULL;
static lv_obj_t *s_time_lbl       = NULL;  /* "elapsed / total" below progress bar */

/* Two poti bars + value labels */
static lv_obj_t *s_bar[2]         = {NULL, NULL};  /* VOL, TMP */
static lv_obj_t *s_val_lbl[2]     = {NULL, NULL};

/* Bypass SoundTouch checkbox → replaced by read-only label */
static lv_obj_t *s_bypass_lbl     = NULL;  /* "1.0x" – shown when fixed_speed_en  */
static bool      s_bypass_active  = false; /* set from song settings, not toggle  */

/* Loop indicator label */
static lv_obj_t *s_loop_lbl       = NULL;  /* "↺ LOOP" – shown when loop configured */

/* Song-config gear button (bottom-right of play screen) */
static lv_obj_t *s_gear_btn       = NULL;

/* Current song context (needed for gear dialog and settings updates) */
static uint16_t  s_current_song_id             = 0;
static char      s_current_song_name[MAX_SONG_NAME_LEN] = "";

/* Tempo lock checkbox */
static lv_obj_t *s_lock_check         = NULL;
static bool      s_tempo_locked       = false;
static uint8_t   s_locked_tempo       = 50;    /* last known / locked poti-scale value */
static uint8_t   s_locked_spd_min_x10 = 4;    /* cached range for label computation   */
static uint8_t   s_locked_spd_max_x10 = 20;

/* Indeterminate progress animation */
static bool      s_prog_anim_active = false;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void prog_anim_exec_cb(void *bar, int32_t val)
{
    lv_bar_set_value((lv_obj_t *)bar, val, LV_ANIM_OFF);
}

static void stop_progress_anim(void)
{
    if (!s_prog_anim_active || !s_progress_bar) return;
    lv_anim_delete(s_progress_bar, prog_anim_exec_cb);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    s_prog_anim_active = false;
}

/** Helper: create one vertical indicator column on the right panel. */
static void create_indicator_col(lv_obj_t *parent,
                                 uint8_t col_idx,
                                 const char *label_text)
{
    /* Column x-offset inside the right panel container */
    lv_coord_t col_x = (lv_coord_t)(col_idx * COL_W);
    lv_coord_t bar_x = col_x + (lv_coord_t)((COL_W - BAR_W) / 2);

    /* Column name label (e.g. "VOL") */
    lv_obj_t *name_lbl = lv_label_create(parent);
    lv_label_set_text(name_lbl, label_text);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_width(name_lbl, COL_W);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(name_lbl, col_x, COLLABEL_Y);

    /* Vertical bar (height > width → LVGL renders it vertically) */
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, BAR_W, BAR_H);
    lv_obj_set_pos(bar, bar_x, BAR_TOP_Y);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    /* Track (background) */
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_BAR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);

    /* Indicator (filled portion) */
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);

    s_bar[col_idx] = bar;

    /* Value text label (e.g. "075") */
    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, "---");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_width(val, COL_W);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(val, col_x, VAL_LABEL_Y);

    s_val_lbl[col_idx] = val;
}

/* =========================================================================
 * Progress bar click callback – runs in LVGL task
 * ========================================================================= */
static void on_progress_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_progress_bar) return;

    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t bar_area;
    lv_obj_get_coords(s_progress_bar, &bar_area);

    int32_t bar_w = bar_area.x2 - bar_area.x1;
    if (bar_w <= 0) return;

    int32_t x_rel = point.x - bar_area.x1;
    if (x_rel < 0)     x_rel = 0;
    if (x_rel > bar_w) x_rel = bar_w;

    uint8_t pct = (uint8_t)(x_rel * 100 / bar_w);
    ESP_LOGI(TAG, "Progress bar clicked: x=%d → seek %u%%", (int)point.x, (unsigned)pct);

    /* Optimistic UI: show the selected position immediately without waiting
     * for the audio player to confirm. Cancel any running animation first
     * (but do NOT reset to 0 like stop_progress_anim() does). */
    if (s_prog_anim_active) {
        lv_anim_delete(s_progress_bar, prog_anim_exec_cb);
        s_prog_anim_active = false;
    }
    lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);

    uart_comm_send_seek(pct);
}

/* =========================================================================
 * Bypass / lock button callbacks – run in LVGL task
 * ========================================================================= */

static void on_bypass_toggled(lv_event_t *e) { (void)e; /* no-op: bypass is now read-only, driven by song config */ }

/* =========================================================================
 * TEMPO LOCK checkbox callback – runs in LVGL task
 * ========================================================================= */
static void on_lock_toggled(lv_event_t *e)
{
    (void)e;
    s_tempo_locked = !s_tempo_locked;

    /* s_locked_tempo already tracks the most recent poti value (updated in
     * async_cb_update_potis while unlocked), so it captures the current
     * running tempo at the moment the user enables lock. */
    uart_comm_send_tempo_lock(s_tempo_locked, s_locked_tempo);

    /* Update TMP bar colour: bypass overrules lock. */
    if (!s_bypass_active) {
        lv_color_t col = s_tempo_locked
            ? lv_color_hex(COLOR_LOCKED)   /* amber – locked  */
            : lv_color_hex(COLOR_ACCENT);  /* cyan  – normal  */
        if (s_bar[1])     lv_obj_set_style_bg_color(s_bar[1], col, LV_PART_INDICATOR);
        if (s_val_lbl[1]) lv_obj_set_style_text_color(s_val_lbl[1], col, 0);
    }

    ESP_LOGI(TAG, "Tempo lock toggled: %s (locked_tempo=%u)",
             s_tempo_locked ? "LOCK" : "UNLOCK", (unsigned)s_locked_tempo);
}

/* =========================================================================
 * Song-config gear button callback – runs in LVGL task
 * ========================================================================= */
static void on_gear_clicked_player(lv_event_t *e)
{
    (void)e;
    if (s_current_song_id != 0) {
        ui_songlist_open_settings_dialog(s_current_song_id);
    }
}

/* =========================================================================
 * STOP button callback – runs in LVGL task
 * ========================================================================= */
static void on_stop_clicked(lv_event_t *e)
{
    (void)e;
    stop_progress_anim();
    uart_comm_send_stop();
    ui_songlist_show(); /* we are in the LVGL task – can call directly */
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */
void ui_player_create(void)
{
    /* ---- Screen -------------------------------------------------------- */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* ---- Left panel (transparent overlay on the screen) --------------- */
    lv_obj_t *left = lv_obj_create(s_screen);
    lv_obj_set_size(left, SPLIT_X, SCREEN_H);
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    /* Music note + title ------------------------------------------------- */
    s_title_lbl = lv_label_create(left);
    lv_label_set_text(s_title_lbl, LV_SYMBOL_AUDIO "  ---");
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title_lbl, SPLIT_X - 2 * PROGRESS_PAD_X);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_pos(s_title_lbl, PROGRESS_PAD_X, TITLE_Y);

    /* "NOW PLAYING" status ------------------------------------------------ */
    lv_obj_t *np_lbl = lv_label_create(left);
    lv_label_set_text(np_lbl, LV_SYMBOL_PLAY "  NOW PLAYING");
    lv_obj_set_style_text_font(np_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(np_lbl, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_pos(np_lbl, PROGRESS_PAD_X, NOWPLAY_Y);

    /* Horizontal divider line -------------------------------------------- */
    static lv_point_precise_t div_pts[2] = {
        {PROGRESS_PAD_X,             PROGRESS_Y - 14},
        {SPLIT_X - PROGRESS_PAD_X,  PROGRESS_Y - 14}
    };
    lv_obj_t *divline = lv_line_create(left);
    lv_line_set_points(divline, div_pts, 2);
    lv_obj_set_style_line_color(divline, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_line_width(divline, 1, 0);

    /* Progress bar (indeterminate animation) ----------------------------- */
    s_progress_bar = lv_bar_create(left);
    lv_obj_set_size(s_progress_bar,
                    SPLIT_X - 2 * PROGRESS_PAD_X, PROGRESS_H);
    lv_obj_set_pos(s_progress_bar, PROGRESS_PAD_X, PROGRESS_Y);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(COLOR_BAR_TRACK),
                               LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_progress_bar, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(COLOR_ACCENT),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 4, LV_PART_INDICATOR);

    /* Make progress bar tappable for seek */
    lv_obj_add_flag(s_progress_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_progress_bar, on_progress_clicked, LV_EVENT_CLICKED, NULL);

    /* Time label: "elapsed / total" right-aligned below the progress bar -- */
    s_time_lbl = lv_label_create(left);
    lv_label_set_text(s_time_lbl, "0:00 / 0:00");
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_width(s_time_lbl, SPLIT_X - 2 * PROGRESS_PAD_X);
    lv_obj_set_style_text_align(s_time_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_time_lbl, PROGRESS_PAD_X, TIME_LABEL_Y);

    /* Loop indicator label – shown when the current song has loop enabled -- */
    s_loop_lbl = lv_label_create(left);
    lv_label_set_text(s_loop_lbl, LV_SYMBOL_REFRESH "  LOOP");
    lv_obj_set_style_text_font(s_loop_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_loop_lbl, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_pos(s_loop_lbl, PROGRESS_PAD_X, STATUS_LBL_Y);
    lv_obj_add_flag(s_loop_lbl, LV_OBJ_FLAG_HIDDEN);  /* hidden until settings arrive */

    /* STOP button -------------------------------------------------------- */
    lv_obj_t *stop_btn = lv_button_create(left);
    lv_obj_set_size(stop_btn, STOP_W, STOP_H);
    lv_obj_set_pos(stop_btn,
                   (SPLIT_X - STOP_W) / 2,  /* horizontally centred */
                   STOP_Y);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(COLOR_STOP_BG), 0);
    lv_obj_set_style_bg_opa(stop_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0xC73652),
                               LV_STATE_PRESSED);
    lv_obj_set_style_radius(stop_btn, 12, 0);
    lv_obj_set_style_border_width(stop_btn, 0, 0);
    lv_obj_set_style_shadow_width(stop_btn, 0, 0);
    lv_obj_add_event_cb(stop_btn, on_stop_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP "  STOP");
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(stop_lbl, lv_color_white(), 0);
    lv_obj_center(stop_lbl);

    /* Song-config gear button – bottom-right of the left panel ----------- */
    s_gear_btn = lv_button_create(left);
    lv_obj_set_size(s_gear_btn, GEAR_BTN_W, GEAR_BTN_H);
    lv_obj_set_pos(s_gear_btn, GEAR_BTN_X, STOP_Y);
    lv_obj_set_style_bg_color(s_gear_btn, lv_color_hex(0x2C3E50), 0);
    lv_obj_set_style_bg_opa(s_gear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_gear_btn, lv_color_hex(0x1A252F), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_gear_btn, 12, 0);
    lv_obj_set_style_border_width(s_gear_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_gear_btn, 0, 0);
    lv_obj_add_event_cb(s_gear_btn, on_gear_clicked_player, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gear_icon = lv_label_create(s_gear_btn);
    lv_label_set_text(gear_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gear_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(gear_icon, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(gear_icon);

    /* ---- Vertical separator between left and right panels -------------- */
    lv_obj_t *sep = lv_obj_create(s_screen);
    lv_obj_set_size(sep, 2, SCREEN_H);
    lv_obj_set_pos(sep, SPLIT_X - 2, 0);
    lv_obj_set_style_bg_color(sep, lv_color_hex(COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);

    /* ---- Right panel (VOL / TMP / EXP bars) ---------------------------- */
    lv_obj_t *right = lv_obj_create(s_screen);
    lv_obj_set_size(right, RIGHT_W, SCREEN_H);
    lv_obj_set_pos(right, SPLIT_X, 0);
    lv_obj_set_style_bg_color(right, lv_color_hex(COLOR_PANEL_R), 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    create_indicator_col(right, 0, "VOL");
    create_indicator_col(right, 1, "TMP");

    /* 1.0x indicator label – read-only, shown when fixed-speed is configured
     * for the current song.  Replaces the old interactive bypass checkbox. -- */
    s_bypass_lbl = lv_label_create(right);
    lv_label_set_text(s_bypass_lbl, "1.0x");
    lv_obj_set_style_text_font(s_bypass_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_bypass_lbl, lv_color_hex(0x445566), 0);
    lv_obj_set_width(s_bypass_lbl, COL_W);
    lv_obj_set_style_text_align(s_bypass_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_bypass_lbl, COL_W, BYPASS_CHECK_Y_R);
    lv_obj_add_flag(s_bypass_lbl, LV_OBJ_FLAG_HIDDEN);  /* hidden until settings arrive */

    /* Tempo Lock checkbox – under VOL bar (col 0), same row as bypass ------ */
    s_lock_check = lv_checkbox_create(right);
    lv_checkbox_set_text(s_lock_check, "Lock");
    lv_obj_set_pos(s_lock_check, 8, BYPASS_CHECK_Y_R);
    lv_obj_set_style_text_font(s_lock_check, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lock_check, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_width(s_lock_check,  30, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_lock_check, 30, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_lock_check, lv_color_hex(COLOR_BAR_TRACK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_lock_check, lv_color_hex(COLOR_LOCKED),
                               LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_lock_check, lv_color_hex(COLOR_LOCKED), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_lock_check, 2, LV_PART_INDICATOR);
    lv_obj_set_style_pad_top(s_lock_check,    10, 0);
    lv_obj_set_style_pad_bottom(s_lock_check, 10, 0);
    lv_obj_set_style_pad_left(s_lock_check,    8, 0);
    lv_obj_set_style_pad_right(s_lock_check,   8, 0);
    lv_obj_add_event_cb(s_lock_check, on_lock_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    ESP_LOGI(TAG, "Player view created");
}

/* =========================================================================
 * lv_async_call callbacks – executed inside the LVGL task
 * ========================================================================= */

typedef struct {
    char song_name[MAX_SONG_NAME_LEN];
} async_show_payload_t;

typedef struct {
    uint8_t volume;
    uint8_t tempo;
    uint8_t speed_min_x10;  /* SPEED_MIN × 10, e.g. 4 → 0.4× */
    uint8_t speed_max_x10;  /* SPEED_MAX × 10, e.g. 20 → 2.0× */
} async_poti_payload_t;

static void async_cb_show(void *user_data)
{
    async_show_payload_t *p = (async_show_payload_t *)user_data;

    /* Store current song context */
    strlcpy(s_current_song_name, p->song_name, MAX_SONG_NAME_LEN);
    s_current_song_id = ui_songlist_find_song_id_by_name(p->song_name);

    /* Reset indicators until settings response arrives */
    s_bypass_active = false;
    if (s_loop_lbl)   lv_obj_add_flag(s_loop_lbl,   LV_OBJ_FLAG_HIDDEN);
    if (s_bypass_lbl) lv_obj_add_flag(s_bypass_lbl, LV_OBJ_FLAG_HIDDEN);
    /* Restore TMP bar to normal (un-bypassed) colour */
    lv_color_t tmp_col = s_tempo_locked
                         ? lv_color_hex(COLOR_LOCKED)
                         : lv_color_hex(COLOR_ACCENT);
    if (s_bar[1])     lv_obj_set_style_bg_color(s_bar[1], tmp_col, LV_PART_INDICATOR);
    if (s_val_lbl[1]) lv_obj_set_style_text_color(s_val_lbl[1], tmp_col, 0);

    /* Request fresh settings so loop / 1.0x indicators are populated */
    if (s_current_song_id != 0) {
        uart_comm_send_song_settings_req(s_current_song_id);
    }

    /* Update title */
    char title_buf[MAX_SONG_NAME_LEN + 6]; /* +6: LV_SYMBOL_AUDIO(3) + "  "(2) + '\0'(1) */
    snprintf(title_buf, sizeof(title_buf), LV_SYMBOL_AUDIO "  %s", p->song_name);
    lv_label_set_text(s_title_lbl, title_buf);

    /* Reset progress – real data arrives immediately via update_progress_async */
    stop_progress_anim();
    if (s_progress_bar) lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    if (s_time_lbl)     lv_label_set_text(s_time_lbl, "0:00 / 0:00");
    lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);

    free(p);
}

static void async_cb_hide(void *user_data)
{
    (void)user_data;
    s_bypass_active   = false;
    s_current_song_id = 0;
    s_current_song_name[0] = '\0';
    /* Hide song-setting indicators */
    if (s_loop_lbl)   lv_obj_add_flag(s_loop_lbl,   LV_OBJ_FLAG_HIDDEN);
    if (s_bypass_lbl) lv_obj_add_flag(s_bypass_lbl, LV_OBJ_FLAG_HIDDEN);
    stop_progress_anim();
    if (s_progress_bar) lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    if (s_time_lbl)     lv_label_set_text(s_time_lbl, "0:00 / 0:00");
    ui_songlist_show();
}

typedef struct {
    uint8_t  pct;
    uint16_t dur_s;
} async_progress_payload_t;

static void async_cb_update_potis(void *user_data)
{
    async_poti_payload_t *p = (async_poti_payload_t *)user_data;
    char buf[12];

    if (s_bar[0]) {
        lv_bar_set_value(s_bar[0], p->volume, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "%3u", p->volume);
        lv_label_set_text(s_val_lbl[0], buf);
    }
    if (s_bar[1]) {
        /* Always cache the latest speed range for the multiplier label. */
        s_locked_spd_min_x10 = p->speed_min_x10;
        s_locked_spd_max_x10 = p->speed_max_x10;

        /* Freeze the displayed tempo when locked AND NOT bypassed.
         * While not frozen, keep s_locked_tempo in sync so it holds the
         * current value the moment the user enables lock.
         * Bypass (1.0x) overrules both the tempo bar and the lock. */
        bool freeze = s_tempo_locked && !s_bypass_active;
        if (!freeze) {
            s_locked_tempo = p->tempo;
        }
        uint8_t display_tempo = s_locked_tempo; /* frozen or just refreshed */

        lv_bar_set_value(s_bar[1], display_tempo, LV_ANIM_ON);
        /* Derive actual speed multiplier from the display value */
        uint32_t speed_x100 = (uint32_t)p->speed_min_x10 * 10
                            + ((uint32_t)(p->speed_max_x10 - p->speed_min_x10) * 10 * display_tempo + 50) / 100;
        snprintf(buf, sizeof(buf), "%u.%02u", (unsigned)(speed_x100 / 100), (unsigned)(speed_x100 % 100));
        lv_label_set_text(s_val_lbl[1], buf);
    }
    free(p);
}

static void async_cb_update_progress(void *user_data)
{
    async_progress_payload_t *p = (async_progress_payload_t *)user_data;

    /* Stop indeterminate animation if still running from a previous session */
    stop_progress_anim();

    if (s_progress_bar) {
        lv_bar_set_value(s_progress_bar, p->pct, LV_ANIM_OFF);
    }
    if (s_time_lbl) {
        uint16_t elapsed_s = (uint16_t)((uint32_t)p->dur_s * p->pct / 100);
        char buf[24];
        snprintf(buf, sizeof(buf), "%u:%02u / %u:%02u",
                 elapsed_s / 60, elapsed_s % 60,
                 p->dur_s  / 60, p->dur_s  % 60);
        lv_label_set_text(s_time_lbl, buf);
    }

    free(p);
}

/* =========================================================================
 * Public async bridges
 * ========================================================================= */

void ui_player_show_async(const char *song_name)
{
    if (!s_screen) {
        ESP_LOGW(TAG, "show_async called before ui_player_create()");
        return;
    }

    async_show_payload_t *p = malloc(sizeof(async_show_payload_t));
    if (!p) { ESP_LOGE(TAG, "OOM in show_async"); return; }

    strlcpy(p->song_name, song_name ? song_name : "", MAX_SONG_NAME_LEN);
    lv_lock();
    lv_async_call(async_cb_show, p);
    lv_unlock();
}

void ui_player_hide_async(void)
{
    if (!s_screen) return;
    lv_lock();
    lv_async_call(async_cb_hide, NULL);
    lv_unlock();
}

void ui_player_update_potis_async(uint8_t volume, uint8_t tempo,
                                  uint8_t speed_min_x10, uint8_t speed_max_x10)
{
    if (!s_screen) return;

    async_poti_payload_t *p = malloc(sizeof(async_poti_payload_t));
    if (!p) { ESP_LOGE(TAG, "OOM in update_potis_async"); return; }

    p->volume        = volume;
    p->tempo         = tempo;
    p->speed_min_x10 = speed_min_x10;
    p->speed_max_x10 = speed_max_x10;
    lv_lock();
    lv_async_call(async_cb_update_potis, p);
    lv_unlock();
}

void ui_player_update_progress_async(uint8_t position_pct, uint16_t duration_s)
{
    if (!s_screen) return;

    async_progress_payload_t *p = malloc(sizeof(async_progress_payload_t));
    if (!p) { ESP_LOGE(TAG, "OOM in update_progress_async"); return; }

    p->pct   = position_pct;
    p->dur_s = duration_s;
    lv_lock();
    lv_async_call(async_cb_update_progress, p);
    lv_unlock();
}

/* =========================================================================
 * Song-settings delivery – async bridge + callback
 * ========================================================================= */

typedef struct {
    uint16_t song_id;
    uint8_t  flags;
    uint8_t  fixed_speed_x100;
} async_player_settings_t;

static void async_cb_song_settings_player(void *user_data)
{
    async_player_settings_t *p = (async_player_settings_t *)user_data;

    /* Only apply settings for the song currently on the player screen */
    if (p->song_id != s_current_song_id || s_current_song_id == 0) {
        free(p);
        return;
    }

    bool loop_en  = (p->flags & 0x01u) != 0;
    bool speed_en = (p->flags & 0x02u) != 0;

    bool prev_bypass = s_bypass_active;
    s_bypass_active  = speed_en;

    /* Update 1.0x indicator */
    if (s_bypass_lbl) {
        if (speed_en) lv_obj_clear_flag(s_bypass_lbl, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(s_bypass_lbl,   LV_OBJ_FLAG_HIDDEN);
    }

    /* Update LOOP indicator */
    if (s_loop_lbl) {
        if (loop_en) lv_obj_clear_flag(s_loop_lbl, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(s_loop_lbl,   LV_OBJ_FLAG_HIDDEN);
    }

    /* Re-tint TMP bar if bypass state changed */
    if (prev_bypass != s_bypass_active) {
        lv_color_t col;
        if (s_bypass_active) {
            col = lv_color_hex(0x445566);       /* muted – fixed speed active   */
        } else if (s_tempo_locked) {
            col = lv_color_hex(COLOR_LOCKED);   /* amber – tempo locked         */
        } else {
            col = lv_color_hex(COLOR_ACCENT);   /* normal cyan                  */
        }
        if (s_bar[1])     lv_obj_set_style_bg_color(s_bar[1], col, LV_PART_INDICATOR);
        if (s_val_lbl[1]) lv_obj_set_style_text_color(s_val_lbl[1], col, 0);
    }

    free(p);
}

void ui_player_song_settings_async(uint16_t song_id, uint8_t flags, uint8_t fixed_speed_x100)
{
    if (!s_screen) return;

    async_player_settings_t *p = malloc(sizeof(async_player_settings_t));
    if (!p) { ESP_LOGE(TAG, "OOM in song_settings_async"); return; }
    p->song_id          = song_id;
    p->flags            = flags;
    p->fixed_speed_x100 = fixed_speed_x100;
    lv_lock();
    lv_async_call(async_cb_song_settings_player, p);
    lv_unlock();
}
