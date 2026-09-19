/**
 * @file buttons.h
 * @brief Assignable physical buttons on the ADC resistor ladder (player mode).
 *
 * A button is identified by its averaged raw ADC value, which is "learned" from the web
 * interface: pick a function, press the button, the averaged value is stored in
 * /sdcard/button_map.json.  Rules:
 *   - one button (ADC value) can only have ONE function per screen context;
 *   - the same button may be used in different contexts (e.g. "Next" on the player screen
 *     and "Down" in the song list).
 *
 * Contexts follow the display: PLAYER while a song is loaded (playing or paused),
 * LIST otherwise.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BTN_CTX_PLAYER = 0,   /* player screen */
    BTN_CTX_LIST   = 1,   /* song list     */
} btn_ctx_t;

typedef enum {
    BTN_FN_PLAYER_PLAYPAUSE = 0,
    BTN_FN_PLAYER_NEXT,
    BTN_FN_PLAYER_PREV,
    BTN_FN_PLAYER_STOP,
    BTN_FN_PLAYER_END_ACTION,   /* cycle end-of-song action: stop -> next -> repeat */
    BTN_FN_LIST_UP,
    BTN_FN_LIST_DOWN,
    BTN_FN_LIST_SELECT,         /* play the highlighted song */
    BTN_FN_COUNT
} btn_fn_t;

typedef struct {
    const char *id;      /* stable identifier used by the web API / JSON file */
    const char *label;   /* human-readable name                               */
    btn_ctx_t   ctx;
} button_fn_info_t;

typedef enum {
    BTN_LEARN_IDLE = 0,
    BTN_LEARN_WAITING,   /* waiting for a button press                   */
    BTN_LEARN_DONE,      /* last learn succeeded                         */
    BTN_LEARN_ERROR,     /* last learn failed (conflict / timeout)       */
} btn_learn_state_t;

typedef struct {
    btn_learn_state_t state;
    int               fn;         /* function being / last learned, -1 if none */
    int               adc;        /* learned value (DONE)                      */
    char              error[80];  /* message (ERROR)                           */
} button_learn_status_t;

/** Create the lock and load /sdcard/button_map.json (call after the SD card is mounted). */
void buttons_load(void);

const button_fn_info_t *buttons_fn_info(int fn);
int  buttons_fn_by_id(const char *id);      /* -1 if unknown */
int  buttons_get_adc(int fn);               /* learned raw ADC value, -1 = unassigned */

/** Function assigned to a button with this averaged raw ADC value in @p ctx, or -1. */
int  buttons_match(int raw_avg, btn_ctx_t ctx);

esp_err_t buttons_clear(int fn);            /* fn = -1 clears every assignment */

/** Record the latest physical press (any mode) so the web UI can show it. fn = -1: unassigned button. */
void buttons_note_press(int raw_avg, int fn);

/** Latest recorded press: sequence number (0 = none yet), matched function (-1 = none), ADC value, age. */
void buttons_last_press_get(uint32_t *seq, int *fn, int *adc, uint32_t *age_ms);

/* ── Learning (started from the web interface, polled from the io task) ───────── */
esp_err_t buttons_learn_start(int fn);      /* arm: the next button press is stored for fn */
void      buttons_learn_cancel(void);       /* stop waiting for a button                   */
bool      buttons_is_learning(void);        /* true while waiting for a press              */
void      buttons_learn_poll(void);         /* call every ~10 ms from the io task          */
void      buttons_learn_get(button_learn_status_t *out);
