/**
 * @file ui_player.c
 * @brief Player view – song title, progress animation, live poti bars, STOP.
 *
 * Layout (800×480):
 *   Left panel  (x=0..599,  w=600): title, "NOW PLAYING", progress bar, STOP button
 *   Divider     (x=597..599, w=2):  vertical separator
 *   Right panel (x=600..799, w=200): VOL / TMP / EXP vertical bar indicators
 *
 * Thread safety: all *_async() functions post lv_async_call() items to the
 * LVGL task.  The STOP button callback and all non-async functions run inside
 * the LVGL task (lv_timer_handler) and may call LVGL APIs directly.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
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
#define PROGRESS_H       22     /* progress bar height                      */
#define PROGRESS_PAD_X   30     /* horizontal padding inside left panel     */
#define STOP_Y           340    /* STOP button top edge                     */
#define STOP_W           300    /* STOP button width                        */
#define STOP_H           90     /* STOP button height                       */

/* Right panel – three indicator columns */
#define COL_W            (RIGHT_W / 3)   /* ≈ 66 px per column              */
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

/* =========================================================================
 * Widget handles
 * ========================================================================= */
static lv_obj_t *s_screen         = NULL;
static lv_obj_t *s_title_lbl      = NULL;
static lv_obj_t *s_progress_bar   = NULL;

/* Three poti bars + value labels */
static lv_obj_t *s_bar[3]         = {NULL, NULL, NULL};  /* VOL, TMP, EXP */
static lv_obj_t *s_val_lbl[3]     = {NULL, NULL, NULL};

/* Indeterminate progress animation */
static lv_anim_t s_prog_anim;
static bool      s_prog_anim_active = false;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void prog_anim_exec_cb(void *bar, int32_t val)
{
    lv_bar_set_value((lv_obj_t *)bar, val, LV_ANIM_OFF);
}

static void start_progress_anim(void)
{
    if (s_prog_anim_active || !s_progress_bar) return;

    lv_anim_init(&s_prog_anim);
    lv_anim_set_var(&s_prog_anim, s_progress_bar);
    lv_anim_set_exec_cb(&s_prog_anim, prog_anim_exec_cb);
    lv_anim_set_values(&s_prog_anim, 0, 100);
    lv_anim_set_duration(&s_prog_anim, 2800);
    lv_anim_set_playback_duration(&s_prog_anim, 2800);
    lv_anim_set_repeat_count(&s_prog_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_prog_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&s_prog_anim);
    s_prog_anim_active = true;
}

static void stop_progress_anim(void)
{
    if (!s_prog_anim_active || !s_progress_bar) return;
    lv_anim_delete(s_progress_bar, prog_anim_exec_cb);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    s_prog_anim_active = false;
}

/** Send CMD_STOP_SONG (no payload) over UART. ISR/task-safe. */
static void send_stop_song(void)
{
    /* [MAGIC 8B][CMD 1B][LEN=0 1B][CHECKSUM 1B] = 11 bytes
     * checksum = CMD_STOP_SONG ^ 0x00 = CMD_STOP_SONG */
    uint8_t pkt[11];
    memcpy(&pkt[0], UART_MAGIC_BYTES, 8);
    pkt[8]  = CMD_STOP_SONG;
    pkt[9]  = 0x00;
    pkt[10] = CMD_STOP_SONG;   /* checksum */
    uart_write_bytes(UART_COMM_PORT, pkt, sizeof(pkt));
    ESP_LOGI(TAG, "CMD_STOP_SONG sent");
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
 * STOP button callback – runs in LVGL task
 * ========================================================================= */
static void on_stop_clicked(lv_event_t *e)
{
    (void)e;
    stop_progress_anim();
    send_stop_song();
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
    create_indicator_col(right, 2, "EXP");

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
    uint8_t expression;
} async_poti_payload_t;

static void async_cb_show(void *user_data)
{
    async_show_payload_t *p = (async_show_payload_t *)user_data;

    /* Update title */
    char title_buf[MAX_SONG_NAME_LEN + 6]; /* +6: LV_SYMBOL_AUDIO(3) + "  "(2) + '\0'(1) */
    snprintf(title_buf, sizeof(title_buf), LV_SYMBOL_AUDIO "  %s", p->song_name);
    lv_label_set_text(s_title_lbl, title_buf);

    /* Reset and start progress animation */
    start_progress_anim();

    /* Load the player screen with a left-slide transition */
    lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);

    free(p);
}

static void async_cb_hide(void *user_data)
{
    (void)user_data;
    stop_progress_anim();
    ui_songlist_show();
}

static void async_cb_update_potis(void *user_data)
{
    async_poti_payload_t *p = (async_poti_payload_t *)user_data;
    char buf[8];

    if (s_bar[0]) {
        lv_bar_set_value(s_bar[0], p->volume, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "%3u", p->volume);
        lv_label_set_text(s_val_lbl[0], buf);
    }
    if (s_bar[1]) {
        lv_bar_set_value(s_bar[1], p->tempo, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "%3u", p->tempo);
        lv_label_set_text(s_val_lbl[1], buf);
    }
    if (s_bar[2]) {
        lv_bar_set_value(s_bar[2], p->expression, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "%3u", p->expression);
        lv_label_set_text(s_val_lbl[2], buf);
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
    lv_async_call(async_cb_show, p);
}

void ui_player_hide_async(void)
{
    if (!s_screen) return;
    lv_async_call(async_cb_hide, NULL);
}

void ui_player_update_potis_async(uint8_t volume, uint8_t tempo, uint8_t expression)
{
    if (!s_screen) return;

    async_poti_payload_t *p = malloc(sizeof(async_poti_payload_t));
    if (!p) { ESP_LOGE(TAG, "OOM in update_potis_async"); return; }

    p->volume     = volume;
    p->tempo      = tempo;
    p->expression = expression;
    lv_async_call(async_cb_update_potis, p);
}
