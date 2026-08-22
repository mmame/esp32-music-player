/**
 * @file ui_songlist.h
 * @brief Songlist UI view for the music-player display.
 *
 * Thread safety
 * -------------
 * All functions that touch LVGL objects MUST be called with the LVGL lock
 * held, OR via lv_async_call() from any other task / core.
 * The uart_comm layer uses lv_async_call() internally, so the public
 * "bridge" functions (ui_songlist_update_async, ui_songlist_encoder_move_async,
 * ui_songlist_encoder_btn_async) are safe to call directly from the UART task.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum songs the list can display at once */
#define SONGLIST_MAX_SONGS  128

/* ---------- Lifecycle ----------------------------------------------------- */

/**
 * @brief Create and display the full-screen songlist view.
 *        Must be called with the LVGL lock held (from LVGL task or app_main
 *        before the LVGL task becomes active).
 */
void ui_songlist_create(void);

/**
 * @brief Load the songlist screen immediately.
 *        Must be called from the LVGL task (e.g. inside a button callback).
 */
void ui_songlist_show(void);

/**
 * @brief Load the songlist screen from any task / core (thread-safe).
 */
void ui_songlist_show_async(void);

/* ---------- UART-task-safe async bridges ---------------------------------- */

/**
 * @brief Schedule a song-list update from any task / core.
 *
 * @param data  Pointer to a buffer of null-terminated song-name strings,
 *              packed consecutively.  The list is terminated by a double
 *              null (i.e. an empty string "").  Each name is prefixed with
 *              a 2-byte little-endian song-ID.
 *
 *              Wire format per entry:
 *                [song_id_lo : u8][song_id_hi : u8][name : char...]['\0']
 *              Terminator:
 *                [0x00 0x00]  (song_id=0 treated as end-of-list sentinel
 *                              when followed by a zero name byte)
 *
 * @param len   Total byte length of the data buffer.
 *
 * The function copies the data internally, so the caller's buffer may be
 * reused immediately after this call returns.
 */
void ui_songlist_update_async(const uint8_t *data, uint16_t len);

/**
 * @brief Schedule an encoder-move event from any task / core.
 *
 * @param steps  Positive = move focus down, negative = move focus up.
 */
void ui_songlist_encoder_move_async(int8_t steps);

/**
 * @brief Schedule an encoder-button press event from any task / core.
 *        Selects the currently focused item and sends CMD_PLAY_SONG.
 */
void ui_songlist_encoder_btn_async(void);

/**
 * @brief Deliver a CMD_SONG_SETTINGS reply from the UART task to the UI.
 *
 * Updates the internal settings cache and, if the settings dialog for
 * @p song_id is currently open, refreshes the checkbox states.
 *
 * Safe to call from any task / core.
 *
 * @param song_id              1-based song index.
 * @param flags                bit0=loop, bit1=fixed_speed_en, bit2=autoplay_next.
 * @param fixed_speed_x100     Fixed speed × 100.
 * @param dimmer_max           Max brightness 0-100.
 * @param dimmer_min           Min brightness 0-100.
 * @param dimmer_rps_ref_x10   Full-brightness RPS × 10.
 * @param dimmer_holdoff_s     Song-position timestamp (s) before which dimmer is suppressed.
 * @param dimmer_fadein_s      Fade-in duration (s) after holdoff expires.
 * @param pitch_influence_pct  Pitch blend factor 0-100.
 */
void ui_songlist_song_settings_async(uint16_t song_id,
                                     uint8_t  flags,
                                     uint8_t  fixed_speed_x100,
                                     uint8_t  dimmer_max,
                                     uint8_t  dimmer_min,
                                     uint8_t  dimmer_rps_ref_x10,
                                     uint8_t  dimmer_holdoff_s,
                                     uint8_t  dimmer_fadein_s,
                                     uint8_t  pitch_influence_pct);

/**
 * @brief Update the BT enable/disable button state from the player's flags.
 *        Safe to call from any task / core.
 */
void ui_songlist_update_bt_enabled_async(bool enabled);

/**
 * @brief Update the WiFi enable/disable button state from the player's flags.
 *        Cancels the auto-disable timer if set to disabled.
 *        Safe to call from any task / core.
 */
void ui_songlist_update_wifi_enabled_async(bool enabled);

/**
 * @brief Open the song-settings dialog for @p song_id on whatever screen is
 *        currently active.  Must be called from the LVGL task (e.g. from a
 *        button callback).  A song_id of 0 is silently ignored.
 */
void ui_songlist_open_settings_dialog(uint16_t song_id);

/**
 * @brief Find a song's ID by its display name.
 *        Must be called from the LVGL task (reads the internal song list).
 *
 * @return song_id (> 0) if found, 0 if not found.
 */
uint16_t ui_songlist_find_song_id_by_name(const char *name);

/**
 * @brief Copy the display name for @p song_id into @p buf.
 *        Must be called from the LVGL task.
 * @return true if found, false if not found (buf is unchanged).
 */
bool ui_songlist_get_song_name(uint16_t song_id, char *buf, size_t buf_len);

/**
 * @brief Return the song ID that follows @p current_id, wrapping at the end.
 *        Must be called from the LVGL task.
 * @return next song_id (> 0), or 0 if the song list is empty.
 */
uint16_t ui_songlist_get_next_song_id(uint16_t current_id);

#ifdef __cplusplus
}
#endif
