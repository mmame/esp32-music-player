/**
 * @file ui_player.h
 * @brief Player view – shown while a song is playing.
 *
 * Screen layout (800×480):
 *
 *   ┌───────────────────────────────────────┬──────────────────┐
 *   │  ♪  SONG TITLE           (Mon. 28)    │  VOL   TMP       │
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
void ui_player_show_async(const char *song_name, uint16_t song_id);

/**
 * @brief Switch back to the songlist view.
 *        Safe to call from any task / core.
 */
void ui_player_hide_async(void);

/**
 * @brief Update the two live poti bar indicators.
 *        Safe to call from any task / core.
 *
 * @param volume        0–100
 * @param tempo         0–100 (mapped from actual speed range by the host)
 * @param speed_min_x10 Minimum speed × 10 (e.g. 4 → 0.4×)
 * @param speed_max_x10 Maximum speed × 10 (e.g. 20 → 2.0×)
 */
void ui_player_update_potis_async(uint8_t volume, uint8_t tempo,
                                  uint8_t speed_min_x10, uint8_t speed_max_x10);

/**
 * @brief Update the progress bar and elapsed/total time label.
 *        Called every ~100 ms while a song is playing.
 *        Safe to call from any task / core.
 *
 * @param position_pct  Current playback position 0–100 %.
 * @param duration_s    Speed-adjusted total song length in seconds.
 */
void ui_player_update_progress_async(uint8_t position_pct, uint16_t duration_s);

/**
 * @brief Deliver song-settings to the player view.
 *        Updates the Loop and 1.0x indicator labels and the fixed-speed
 *        (bypass) state for the TMP bar colouring.
 *        Safe to call from any task / core.
 *
 * @param song_id            1-based song index.
 * @param flags              bit0=loop, bit1=fixed_speed_en, bit3=dimmer_override.
 * @param fixed_speed_x100   Fixed speed × 100.
 * @param dimmer_max         Max brightness 0-100.
 * @param dimmer_min         Min brightness 0-100.
 * @param dimmer_rps_ref_x10 Full-brightness RPS × 10.
 * @param dimmer_holdoff_s   Seconds before dimmer activates.
 * @param pitch_influence_pct Pitch blend factor 0-100.
 */
void ui_player_song_settings_async(uint16_t song_id,
                                   uint8_t  flags,
                                   uint8_t  fixed_speed_x100,
                                   uint8_t  dimmer_max,
                                   uint8_t  dimmer_min,
                                   uint8_t  dimmer_rps_ref_x10,
                                   uint8_t  dimmer_holdoff_s,
                                   uint8_t  pitch_influence_pct);

/**
 * @brief Update the speed-lock ("HOLD") indicator.
 *        Called every ~100 ms with the current hardware switch state.
 *        Safe to call from any task / core.
 *
 * @param locked  true = switch active, speed is held; false = free-running.
 */
void ui_player_update_speed_locked_async(bool locked);

#ifdef __cplusplus
}
#endif
