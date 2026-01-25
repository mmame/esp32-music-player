#include "about_ui.h"
#include "button_config_ui.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include <stdio.h>

static const char *TAG = "About";

static lv_obj_t *about_screen = NULL;

extern "C" {

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
    lv_label_set_text(title, "About");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // Create info container
    lv_obj_t *info_container = lv_obj_create(about_screen);
    lv_obj_set_size(info_container, 700, 350);
    lv_obj_align(info_container, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(info_container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(info_container, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(info_container, 2, 0);
    lv_obj_set_style_pad_all(info_container, 20, 0);
    lv_obj_clear_flag(info_container, LV_OBJ_FLAG_SCROLLABLE);

    // Device name
    lv_obj_t *device_label = lv_label_create(info_container);
    lv_label_set_text(device_label, "ESP32-8048S050C");
    lv_obj_set_style_text_font(device_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(device_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(device_label, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *subtitle_label = lv_label_create(info_container);
    lv_label_set_text(subtitle_label, "Music Player");
    lv_obj_set_style_text_font(subtitle_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(subtitle_label, LV_ALIGN_TOP_MID, 0, 45);

    // Build info
    lv_obj_t *build_label = lv_label_create(info_container);
    char build_info[128];
    snprintf(build_info, sizeof(build_info), "Build Date: %s %s", __DATE__, __TIME__);
    lv_label_set_text(build_label, build_info);
    lv_obj_set_style_text_font(build_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(build_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(build_label, LV_ALIGN_TOP_LEFT, 10, 90);

    // ESP-IDF version
    lv_obj_t *idf_label = lv_label_create(info_container);
    char idf_info[64];
    snprintf(idf_info, sizeof(idf_info), "ESP-IDF: %s", IDF_VER);
    lv_label_set_text(idf_label, idf_info);
    lv_obj_set_style_text_font(idf_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(idf_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(idf_label, LV_ALIGN_TOP_LEFT, 10, 120);

    // Chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    lv_obj_t *chip_label = lv_label_create(info_container);
    char chip_str[64];
    snprintf(chip_str, sizeof(chip_str), "Chip: ESP32-S3 (rev %d)", chip_info.revision);
    lv_label_set_text(chip_label, chip_str);
    lv_obj_set_style_text_font(chip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(chip_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(chip_label, LV_ALIGN_TOP_LEFT, 10, 150);

    lv_obj_t *cores_label = lv_label_create(info_container);
    char cores_str[64];
    snprintf(cores_str, sizeof(cores_str), "CPU Cores: %d", chip_info.cores);
    lv_label_set_text(cores_label, cores_str);
    lv_obj_set_style_text_font(cores_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cores_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(cores_label, LV_ALIGN_TOP_LEFT, 10, 180);

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
    lv_obj_align(features_label, LV_ALIGN_TOP_LEFT, 10, 210);

    // PSRAM info
    lv_obj_t *psram_label = lv_label_create(info_container);
    lv_label_set_text(psram_label, "PSRAM: 8 MB");
    lv_obj_set_style_text_font(psram_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(psram_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_align(psram_label, LV_ALIGN_TOP_LEFT, 10, 240);

    // Navigation hint
    lv_obj_t *nav_hint = lv_label_create(about_screen);
    lv_label_set_text(nav_hint, "Swipe RIGHT for Button Config");
    lv_obj_set_style_text_font(nav_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nav_hint, lv_color_hex(0x666666), 0);
    lv_obj_align(nav_hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// Initialize about UI
void about_ui_init(void)
{
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
