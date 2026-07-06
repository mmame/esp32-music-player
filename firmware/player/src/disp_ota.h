/**
 * @file disp_ota.h
 * @brief Remote firmware-update for the display ESP32 via UART1 + ROM bootloader.
 *
 * Normal operation pin states (call disp_ota_init() once at startup):
 *   DISP_ESP32_RESET_PIN (GPIO9)  = HIGH  → reset deasserted (running)
 *   DISP_ESP32_BOOT0_PIN (GPIO2)  = LOW   → GPIO0 of display: normal boot
 *
 * Bootloader entry:
 *   1. BOOT0_PIN → HIGH  (active-high → display GPIO0 selects ROM bootloader)
 *   2. RST_PIN   → LOW → HIGH  (reset pulse)
 *   3. Display ESP32 starts in UART ROM bootloader at 115200 baud
 *
 * The flash operation temporarily pauses uart_master (UART1 is reused at
 * 115200 baud for the ROM protocol), then resumes normal comms afterwards.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configure DISP_ESP32_RESET_PIN and DISP_ESP32_BOOT0_PIN as outputs and
 * set them to the normal-operation state (RST=HIGH, BOOT0=LOW).
 *
 * Must be called once early in app_main, before uart_master_init().
 */
void disp_ota_init(void);

/**
 * Flash a firmware binary from the SD card to the display ESP32.
 *
 * Sequence:
 *   - Pauses uart_master (UART1 communication with display)
 *   - Toggles RST/BOOT0 to enter ROM UART bootloader
 *   - Performs SLIP-framed ESP32 ROM bootloader protocol:
 *       SYNC → SPI_ATTACH → CHANGE_BAUDRATE → FLASH_BEGIN → FLASH_DATA × N → FLASH_END
 *   - Resets the display back to normal boot
 *   - Resumes uart_master
 *
 * Progress messages are sent as HTTP chunked text to @p req (if non-NULL).
 * The final terminating chunk (NULL) is sent by this function on both
 * success and failure.
 *
 * @param path        Absolute SD-card path to the firmware .bin file.
 * @param flash_addr  Target flash address (e.g., 0x10000 for app partition).
 * @param req         HTTP request for streaming progress output; may be NULL.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t disp_ota_flash(const char *path, uint32_t flash_addr, httpd_req_t *req);

#ifdef __cplusplus
}
#endif
