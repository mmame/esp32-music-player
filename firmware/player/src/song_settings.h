#pragma once

#include <stdbool.h>
#include <stdint.h>

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
 *   "loop"              : boolean – restart the song from the beginning when it ends.
 *   "fixed_speed"       : number  – play at this speed multiplier regardless of crank speed.
 *   "pitch_influence"   : number  – pitch blend factor 0-100 (0=time-stretch, 100=tape effect).
 *   "dimmer_max"        : number  – max brightness 0-100 (default 100).
 *   "dimmer_min"        : number  – min brightness 0-100 (default 0).
 *   "dimmer_rps_ref"    : number  – RPS at which full brightness is reached (default 1.4).
 *   "dimmer_holdoff_s"  : number  – song-position timestamp (s) before which dimmer is suppressed (default 0).
 *   "dimmer_fadein_s"   : number  – seconds to fade from 0 to full brightness when holdoff expires (default 0).
 *   "light_organ"       : boolean – drive dimmer brightness from audio FFT energy instead of crank speed.
 */

typedef struct {
    bool    loop;            /**< true: restart automatically when song ends           */
    float   fixed_speed;     /**< 0.0f = follow crank; >0.0f = locked speed            */
    uint8_t pitch_influence; /**< pitch blend 0-100: 0=time-stretch, 100=full tape effect */
    uint8_t dimmer_max;      /**< max brightness 0-100                                 */
    uint8_t dimmer_min;      /**< min brightness 0-100                                 */
    float   dimmer_rps_ref;  /**< RPS at which full brightness is reached              */
    uint8_t dimmer_holdoff_s;/**< song-position timestamp (s) before which dimmer is suppressed */
    uint8_t dimmer_fadein_s; /**< seconds to fade from 0→full brightness after holdoff expires  */
    bool    light_organ;     /**< true: dimmer driven by FFT audio energy instead of crank speed */
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
