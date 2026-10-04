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

/** Flash address meaning "full image: choose by detected chip" (see disp_ota_flash()). */
#define DISP_OTA_ADDR_AUTO  0xFFFFFFFFu

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
 * @param flash_addr  Target flash address, or DISP_OTA_ADDR_AUTO to pick it from the detected chip:
 *                    0x1000 (ESP32) / 0x0 (ESP32-S3) = start of a merged full image
 *                    (bootloader + partition table + app).  0x10000 flashes the app only.
 * @param req         HTTP request for streaming progress output; may be NULL.
 * @param log_ms      If > 0, after the final display reset the display's boot log (UART0 console,
 *                    115200 baud) is captured for this many ms and streamed into @p req.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t disp_ota_flash(const char *path, uint32_t flash_addr, httpd_req_t *req, uint32_t log_ms);

/**
 * Reset the display (normal boot) and stream its boot log (console UART, 115200 baud) into @p req
 * for @p duration_ms.  Pauses/resumes uart_master around the capture.  Does not send the
 * terminating chunk.
 */
esp_err_t disp_ota_capture_log(httpd_req_t *req, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
