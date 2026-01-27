#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Current firmware version
#define FIRMWARE_VERSION "1.0.0"

// GitHub release URLs
#define GITHUB_RELEASE_URL "https://github.com/mmame/esp32-music-player/releases/latest/download/firmware.bin"
#define GITHUB_VERSION_URL "https://github.com/mmame/esp32-music-player/releases/latest/download/version.json"

// OTA update status
typedef enum {
    OTA_STATUS_IDLE,
    OTA_STATUS_CHECKING,
    OTA_STATUS_UPDATE_AVAILABLE,
    OTA_STATUS_NO_UPDATE,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_INSTALLING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_ERROR
} ota_status_t;

// OTA update callback for progress
typedef void (*ota_progress_callback_t)(int progress, const char *message);

/**
 * @brief Initialize OTA update system
 * 
 * @return true if initialization successful, false otherwise
 */
bool ota_update_init(void);

/**
 * @brief Check for available firmware updates
 * 
 * @param callback Progress callback function
 * @return true if update is available, false otherwise
 */
bool ota_check_for_updates(ota_progress_callback_t callback);

/**
 * @brief Perform OTA firmware update
 * 
 * @param callback Progress callback function
 * @return true if update successful, false otherwise
 */
bool ota_perform_update(ota_progress_callback_t callback);

/**
 * @brief Get current OTA status
 * 
 * @return Current OTA status
 */
ota_status_t ota_get_status(void);

/**
 * @brief Get current firmware version
 * 
 * @return Version string
 */
const char* ota_get_current_version(void);

/**
 * @brief Get available firmware version (after checking)
 * 
 * @return Version string or NULL if not checked
 */
const char* ota_get_available_version(void);

/**
 * @brief Get last error message
 * 
 * @return Error message string
 */
const char* ota_get_error_message(void);

#ifdef __cplusplus
}
#endif

#endif // OTA_UPDATE_H
