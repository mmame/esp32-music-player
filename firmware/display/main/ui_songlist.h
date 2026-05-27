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
 * @param song_id          1-based song index.
 * @param flags            Bit 0 = loop, bit 1 = fixed_speed_en.
 * @param fixed_speed_x100 Fixed speed × 100 (e.g. 100 = 1.0×).
 */
void ui_songlist_song_settings_async(uint16_t song_id,
                                     uint8_t  flags,
                                     uint8_t  fixed_speed_x100);

#ifdef __cplusplus
}
#endif
