#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked after any file operation (upload, rename, delete)
 * so the player can rescan its playlist.
 */
typedef void (*rescan_cb_t)(void);

/**
 * Callback invoked after the browser saves song settings for a given WAV file.
 * Called from the HTTP-server task with the parsed settings, so that the player
 * can live-apply them to the currently running song without an SD-card re-read.
 * @param wav_path  Full path, e.g. "/sdcard/foo.wav".
 */
typedef void (*web_song_settings_cb_t)(const char *wav_path,
                                       bool        loop,
                                       bool        autoplay_next,
                                       float       fixed_speed,
                                       uint8_t     pitch_influence,
                                       uint8_t     dimmer_max,
                                       uint8_t     dimmer_min,
                                       float       dimmer_rps_ref,
                                       uint8_t     dimmer_holdoff_s,
                                       uint8_t     dimmer_fadein_s,
                                       uint8_t     downmix_mode,
                                       uint8_t     downmix_fade_s);

/**
 * Initialise the WiFi stack (netif, event loop, esp_wifi_init) and store the
 * rescan callback.  Does NOT start the AP or HTTP server.
 * Call once early in app_main, after NVS and SD card are initialised.
 *
 * @param on_files_changed  Called on the HTTP-server task whenever a WAV
 *                          file is added, removed, or renamed on the SD card.
 *                          May be NULL.
 */
void web_server_init(rescan_cb_t on_files_changed);

/**
 * Register a callback to receive live song-settings updates from the browser.
 * Call once from app_main before web_server_enable().
 */
void web_server_set_song_settings_callback(web_song_settings_cb_t cb);

/**
 * Start the WiFi soft-AP and the HTTP file-manager server.
 * Safe to call from any task.  No-op if already running.
 */
void web_server_enable(void);

/**
 * Stop the HTTP server and the WiFi soft-AP.
 * Safe to call from any task.  No-op if already stopped.
 */
void web_server_disable(void);

/**
 * Returns true if the WiFi AP and HTTP server are currently running.
 */
bool web_server_is_running(void);

#ifdef __cplusplus
}
#endif
