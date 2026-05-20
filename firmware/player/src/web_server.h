#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback invoked after any file operation (upload, rename, delete)
 * so the player can rescan its playlist.
 */
typedef void (*rescan_cb_t)(void);

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
 * Start the WiFi soft-AP and the HTTP file-manager server.
 * Safe to call from any task.  No-op if already running.
 */
void web_server_enable(void);

/**
 * Stop the HTTP server and the WiFi soft-AP.
 * Safe to call from any task.  No-op if already stopped.
 */
void web_server_disable(void);

#ifdef __cplusplus
}
#endif
