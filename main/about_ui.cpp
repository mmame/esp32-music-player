#include "about_ui.h"
#include "button_config_ui.h"
#include "ota_update.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <stdio.h>

static const char *TAG = "About";

static lv_obj_t *about_screen = NULL;
static lv_obj_t *update_button = NULL;
static lv_obj_t *progress_msgbox = NULL;
static lv_obj_t *confirm_msgbox = NULL;
static lv_obj_t *progress_bar = NULL;
static lv_obj_t *progress_label = NULL;

extern "C" {

// Forward declarations
static void check_update_event_cb(lv_event_t *e);
static void update_confirm_event_cb(lv_event_t *e);
static bool is_wifi_sta_connected(void);

// Gesture event callback for swipe navigation
static void about_gesture_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        
        if (dir == LV_DIR_RIGHT) {
            ESP_LOGI(TAG, "Swipe RIGHT detected, going back to button config");
            button_config_show();
        }
    }
}

// Create the about UI
static void create_about_ui(void)
{
    about_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(about_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_add_flag(about_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(about_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Add gesture event for swipe navigation
    lv_obj_add_event_cb(about_screen, about_gesture_event_cb, LV_EVENT_GESTURE, NULL);

    // Title
    lv_obj_t *title = lv_label_create(about_screen);
    lv_label_set_text(title, "ESP32 Music Player");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // Page indicators (5 circles)
    for (int i = 0; i < 5; i++) {
        lv_obj_t *circle = lv_obj_create(about_screen);
        lv_obj_set_size(circle, 12, 12);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(circle, 2, 0);
        lv_obj_set_style_border_color(circle, lv_color_hex(0x00FF00), 0);
        if (i == 0) {
            lv_obj_set_style_bg_color(circle, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);
        }
        lv_obj_align(circle, LV_ALIGN_TOP_RIGHT, -10 - (i * 18), 12);
    }

    // Create info container
    lv_obj_t *info_container = lv_obj_create(about_screen);
    lv_obj_set_size(info_container, 700, 350);
    lv_obj_align(info_container, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(info_container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(info_container, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(info_container, 2, 0);
    lv_obj_set_style_pad_all(info_container, 20, 0);
    lv_obj_clear_flag(info_container, LV_OBJ_FLAG_SCROLLABLE);

    // GitHub URL
    lv_obj_t *github_label = lv_label_create(info_container);
    lv_label_set_text(github_label, "GitHub:");
    lv_obj_set_style_text_font(github_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(github_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(github_label, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *url_label = lv_label_create(info_container);
    lv_label_set_text(url_label, "https://github.com/mmame/esp32-music-player");
    lv_obj_set_style_text_font(url_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(url_label, lv_color_hex(0x00AAFF), 0);
    lv_obj_align(url_label, LV_ALIGN_TOP_MID, 0, 35);

    // Device note
    lv_obj_t *device_note = lv_label_create(info_container);
    lv_label_set_text(device_note, "Device: ESP32-8048S050C");
    lv_obj_set_style_text_font(device_note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(device_note, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(device_note, LV_ALIGN_TOP_MID, 0, 58);

    // Build info
    lv_obj_t *build_label = lv_label_create(info_container);
    char build_info[128];
    snprintf(build_info, sizeof(build_info), "Build Date: %s %s", __DATE__, __TIME__);
    lv_label_set_text(build_label, build_info);
    lv_obj_set_style_text_font(build_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(build_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(build_label, LV_ALIGN_TOP_LEFT, 10, 70);

    // ESP-IDF version
    lv_obj_t *idf_label = lv_label_create(info_container);
    char idf_info[64];
    snprintf(idf_info, sizeof(idf_info), "ESP-IDF: %s", IDF_VER);
    lv_label_set_text(idf_label, idf_info);
    lv_obj_set_style_text_font(idf_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(idf_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(idf_label, LV_ALIGN_TOP_LEFT, 10, 100);

    // Chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    lv_obj_t *chip_label = lv_label_create(info_container);
    char chip_str[64];
    snprintf(chip_str, sizeof(chip_str), "Chip: ESP32-S3 (rev %d)", chip_info.revision);
    lv_label_set_text(chip_label, chip_str);
    lv_obj_set_style_text_font(chip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(chip_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(chip_label, LV_ALIGN_TOP_LEFT, 10, 130);

    lv_obj_t *cores_label = lv_label_create(info_container);
    char cores_str[64];
    snprintf(cores_str, sizeof(cores_str), "CPU Cores: %d", chip_info.cores);
    lv_label_set_text(cores_label, cores_str);
    lv_obj_set_style_text_font(cores_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cores_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(cores_label, LV_ALIGN_TOP_LEFT, 10, 160);

    // Features
    lv_obj_t *features_label = lv_label_create(info_container);
    char features_str[128];
    snprintf(features_str, sizeof(features_str), "Features: WiFi%s%s%s",
             (chip_info.features & CHIP_FEATURE_BT) ? " + BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? " + BLE" : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? " + 802.15.4" : "");
    lv_label_set_text(features_label, features_str);
    lv_obj_set_style_text_font(features_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(features_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(features_label, LV_ALIGN_TOP_LEFT, 10, 190);

    // PSRAM info
    lv_obj_t *psram_label = lv_label_create(info_container);
    lv_label_set_text(psram_label, "PSRAM: 8 MB");
    lv_obj_set_style_text_font(psram_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(psram_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(psram_label, LV_ALIGN_TOP_LEFT, 10, 220);
    
    // Current version
    lv_obj_t *version_label = lv_label_create(info_container);
    char version_str[64];
    snprintf(version_str, sizeof(version_str), "Version: %s", ota_get_current_version());
    lv_label_set_text(version_label, version_str);
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(version_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(version_label, LV_ALIGN_TOP_LEFT, 10, 250);
    
    // Check for Updates button
    update_button = lv_btn_create(info_container);
    lv_obj_set_size(update_button, 200, 50);
    lv_obj_align(update_button, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(update_button, lv_color_hex(0x00AA00), LV_PART_MAIN);
    lv_obj_add_event_cb(update_button, check_update_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_label = lv_label_create(update_button);
    lv_label_set_text(btn_label, "Check for Updates");
    lv_obj_center(btn_label);
}

// Progress callback for OTA
static void ota_progress_cb(int progress, const char *message)
{
    lv_lock();
    if (progress_bar && progress_label) {
        lv_bar_set_value(progress_bar, progress, LV_ANIM_OFF);
        lv_label_set_text(progress_label, message);
    }
    lv_unlock();
}

// Task to perform OTA check
static void ota_check_task(void *arg)
{
    bool update_available = ota_check_for_updates(ota_progress_cb);
    
    vTaskDelay(pdMS_TO_TICKS(500)); // Brief delay to show completion
    
    // Lock LVGL for thread-safe UI operations
    lv_lock();
    
    // Close progress dialog
    if (progress_msgbox) {
        lv_msgbox_close(progress_msgbox);
        progress_msgbox = NULL;
    }
    
    if (update_available) {
        // Show update confirmation dialog
        char msg[256];
        snprintf(msg, sizeof(msg), "New version %s is available!\n\nCurrent: %s\nNew: %s\n\nUpdate now?",
                ota_get_available_version(),
                ota_get_current_version(),
                ota_get_available_version());
        
        confirm_msgbox = lv_msgbox_create(about_screen);
        lv_msgbox_add_title(confirm_msgbox, "Update Available");
        lv_msgbox_add_text(confirm_msgbox, msg);
        lv_msgbox_add_close_button(confirm_msgbox);
        lv_obj_t *update_btn = lv_msgbox_add_footer_button(confirm_msgbox, "Update");
        lv_obj_add_event_cb(update_btn, update_confirm_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_size(confirm_msgbox, 600, 300);
        lv_obj_center(confirm_msgbox);
    } else {
        ota_status_t status = ota_get_status();
        const char *msg = (status == OTA_STATUS_NO_UPDATE) ? 
            "You have the latest version!" : 
            ota_get_error_message();
        
        lv_obj_t *msgbox = lv_msgbox_create(about_screen);
        lv_msgbox_add_title(msgbox, "Update Check");
        lv_msgbox_add_text(msgbox, msg);
        lv_msgbox_add_close_button(msgbox);
        lv_obj_set_size(msgbox, 500, 200);
        lv_obj_center(msgbox);
    }
    
    // Unlock LVGL
    lv_unlock();
    
    vTaskDelete(NULL);
}

// Task to perform OTA update
static void ota_update_task(void *arg)
{
    bool success = ota_perform_update(ota_progress_cb);
    
    vTaskDelay(pdMS_TO_TICKS(2000)); // Show completion message
    
    // Lock LVGL for thread-safe UI operations
    lv_lock();
    
    if (progress_msgbox) {
        lv_msgbox_close(progress_msgbox);
        progress_msgbox = NULL;
    }
    
    lv_unlock();
    
    if (success) {
        ESP_LOGI(TAG, "OTA update successful, rebooting in 3 seconds...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        lv_lock();
        lv_obj_t *msgbox = lv_msgbox_create(about_screen);
        lv_msgbox_add_title(msgbox, "Update Failed");
        lv_msgbox_add_text(msgbox, ota_get_error_message());
        lv_msgbox_add_close_button(msgbox);
        lv_obj_set_size(msgbox, 500, 200);
        lv_obj_center(msgbox);
        lv_unlock();
    }
    
    vTaskDelete(NULL);
}

// Event handler for update confirmation
static void update_confirm_event_cb(lv_event_t *e)
{
    // Close confirmation dialog
    if (confirm_msgbox) {
        lv_msgbox_close(confirm_msgbox);
        confirm_msgbox = NULL;
    }
    
    // Show progress dialog
    progress_msgbox = lv_msgbox_create(about_screen);
    lv_msgbox_add_title(progress_msgbox, "Updating Firmware");
    lv_obj_set_size(progress_msgbox, 600, 250);
    lv_obj_center(progress_msgbox);
        
        lv_obj_t *content = lv_msgbox_get_content(progress_msgbox);
        
        progress_bar = lv_bar_create(content);
        lv_obj_set_size(progress_bar, 500, 30);
        lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 20);
        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
        
        progress_label = lv_label_create(content);
        lv_label_set_text(progress_label, "Starting update...");
        lv_obj_align(progress_label, LV_ALIGN_TOP_MID, 0, 70);
    
    // Start OTA update task
    xTaskCreate(ota_update_task, "ota_update", 8192, NULL, 5, NULL);
}

// Helper function to check if WiFi is in STA mode and connected
static bool is_wifi_sta_connected(void)
{
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    
    // Check if WiFi is initialized and in STA or APSTA mode
    if (err != ESP_OK || (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA)) {
        return false;
    }
    
    // Check if we're actually connected
    wifi_ap_record_t ap_info;
    err = esp_wifi_sta_get_ap_info(&ap_info);
    
    return (err == ESP_OK);
}

// Event handler for check update button
static void check_update_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Check for updates button clicked");
    
    // Check WiFi STA connection first
    if (!is_wifi_sta_connected()) {
        ESP_LOGW(TAG, "WiFi not connected in STA mode");
        lv_obj_t *msgbox = lv_msgbox_create(about_screen);
        lv_msgbox_add_title(msgbox, "WiFi Required");
        lv_msgbox_add_text(msgbox, 
            "WiFi connection required to check for updates.\n\n"
            "Please go to the 'WiFi Configuration' tab\n"
            "and connect to a WiFi network in STA mode.");
        lv_msgbox_add_close_button(msgbox);
        lv_obj_set_size(msgbox, 550, 250);
        lv_obj_center(msgbox);
        return;
    }
    
    // Show progress dialog
    progress_msgbox = lv_msgbox_create(about_screen);
    lv_msgbox_add_title(progress_msgbox, "Checking for Updates");
    lv_msgbox_add_close_button(progress_msgbox);
    lv_obj_set_size(progress_msgbox, 500, 200);
    lv_obj_center(progress_msgbox);
    
    lv_obj_t *content = lv_msgbox_get_content(progress_msgbox);
    
    progress_bar = lv_bar_create(content);
    lv_obj_set_size(progress_bar, 400, 30);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 20);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    
    progress_label = lv_label_create(content);
    lv_label_set_text(progress_label, "Connecting to GitHub...");
    lv_obj_align(progress_label, LV_ALIGN_TOP_MID, 0, 70);
    
    // Start check task
    xTaskCreate(ota_check_task, "ota_check", 8192, NULL, 5, NULL);
}

// Initialize about UI
void about_ui_init(void)
{
    // Initialize OTA update system
    ota_update_init();
    
    create_about_ui();
    ESP_LOGI(TAG, "About UI initialized");
}

// Show about screen
void about_show(void)
{
    if (about_screen) {
        lv_screen_load(about_screen);
        ESP_LOGI(TAG, "About screen shown");
    }
}

// Hide about screen
void about_hide(void)
{
    ESP_LOGI(TAG, "About screen hidden");
}

// Get about screen
lv_obj_t * about_get_screen(void)
{
    return about_screen;
}

} // extern "C"
