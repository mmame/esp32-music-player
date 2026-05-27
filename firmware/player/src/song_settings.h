#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file song_settings.h
 * @brief Optional per-song settings loaded from a JSON sidecar file.
 *
 * For a WAV file at "/sdcard/foo.wav" the player looks for "/sdcard/foo.json".
 * If the file is absent the defaults (no loop, no fixed speed) apply silently.
 *
 * Supported JSON keys (all optional):
 *   "loop"        : boolean – restart the song from the beginning when it ends,
 *                   until Stop is pressed.
 *   "fixed_speed" : number  – play at this speed multiplier regardless of crank
 *                   speed (e.g. 1.0 for normal speed, 1.2 for 20 % faster).
 *                   Omitting the key (or setting it to 0) restores crank control.
 *
 * Example:
 *   { "loop": true, "fixed_speed": 1.0 }
 */

typedef struct {
    bool  loop;          /**< true: restart automatically when song ends  */
    float fixed_speed;   /**< 0.0f = follow crank; >0.0f = locked speed   */
} song_settings_t;

/**
 * Load settings for the given WAV file path.
 *
 * Replaces the ".wav" extension with ".json" and tries to open that file.
 * If the file is absent, cannot be opened, or contains invalid JSON the
 * function fills @p out with safe defaults and returns without error.
 *
 * @param wav_path  Absolute path of the WAV file, e.g. "/sdcard/foo.wav".
 * @param out       Caller-provided struct to receive the settings.
 */
void song_settings_load(const char *wav_path, song_settings_t *out);

#ifdef __cplusplus
}
#endif
