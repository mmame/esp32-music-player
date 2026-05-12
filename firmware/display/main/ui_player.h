/**
 * @file ui_player.h
 * @brief Player view – shown while a song is playing.
 *
 * Screen layout (800×480):
 *
 *   ┌───────────────────────────────────────┬──────────────────┐
 *   │  ♪  SONG TITLE           (Mon. 28)    │  VOL   TMP   EXP │
 *   │                                       │  ███   ███   ███  │
 *   │  NOW PLAYING ●                        │  ███   ███   ███  │
 *   │                                       │  ███   ███   ███  │
 *   │  [████████████████░░░░░░░░░░░░░░░░]  │  ███   ███   ███  │
 *   │                                       │                   │
 *   │           [       STOP       ]        │  075   120   080  │
 *   └───────────────────────────────────────┴──────────────────┘
 *         0                    600                           800
 *
 * Thread safety
 * -------------
 * Functions suffixed _async() are safe to call from any task / core.
 * All other functions must be called with the LVGL lock held.
 */
#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Lifecycle ----------------------------------------------------- */

/**
 * @brief Create all player-view widgets (screen is NOT loaded).
 *        Must be called once at startup with the LVGL lock held, after
 *        ui_songlist_create() so the songlist is the initial active screen.
 */
void ui_player_create(void);

/* ---------- UART-task-safe async bridges ---------------------------------- */

/**
 * @brief Switch to the player view and display the given song name.
 *        Safe to call from any task / core.
 *
 * @param song_name  Null-terminated UTF-8 string (copied internally).
 */
void ui_player_show_async(const char *song_name);

/**
 * @brief Switch back to the songlist view.
 *        Safe to call from any task / core.
 */
void ui_player_hide_async(void);

/**
 * @brief Update the three live poti bar indicators.
 *        Safe to call from any task / core.
 *
 * @param volume      0–100
 * @param tempo       0–100 (mapped from actual BPM range by the host)
 * @param expression  0–100
 */
void ui_player_update_potis_async(uint8_t volume, uint8_t tempo, uint8_t expression);

#ifdef __cplusplus
}
#endif
