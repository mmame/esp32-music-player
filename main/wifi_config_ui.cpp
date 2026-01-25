#include "wifi_config_ui.h"
#include "webserver.h"
#include "audio_player_ui.h"
#include "file_manager_ui.h"
#include "button_config_ui.h"
#include "sunton_esp32_8048s050c.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include "esp_mac.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

static const char *TAG = "WiFiConfig";

#define WIFI_AP_SSID_DEFAULT "ESP32-MusicPlayer"
#define WIFI_AP_PASS_DEFAULT "music2026"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4
#define MOUNT_POINT "/sdcard"
#define NVS_NAMESPACE "wifi_config"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64
#define MAX_STA_RETRY 5

// WiFi mode selection
typedef enum {
    WIFI_UI_MODE_AP,
    WIFI_UI_MODE_STA
} wifi_ui_mode_t;

// UI elements
static lv_obj_t *wifi_config_screen = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *mode_dropdown = NULL;
static lv_obj_t *sta_container = NULL;
static lv_obj_t *ssid_textarea = NULL;
static lv_obj_t *password_textarea = NULL;
static lv_obj_t *connect_btn = NULL;
static lv_obj_t *keyboard = NULL;
static lv_obj_t *ap_start_btn = NULL;
static lv_obj_t *ap_container = NULL;
static lv_obj_t *ap_ssid_textarea = NULL;
static lv_obj_t *ap_password_textarea = NULL;
static lv_obj_t *sta_password_toggle_btn = NULL;
static lv_obj_t *ap_password_toggle_btn = NULL;

// WiFi state
static wifi_ui_mode_t current_ui_mode = WIFI_UI_MODE_AP;
static wifi_mode_t current_wifi_mode = WIFI_MODE_AP;
static char sta_ssid[MAX_SSID_LEN] = "";
static char sta_password[MAX_PASS_LEN] = "";
static char ap_ssid[MAX_SSID_LEN] = WIFI_AP_SSID_DEFAULT;
static char ap_password[MAX_PASS_LEN] = WIFI_AP_PASS_DEFAULT;
static bool wifi_initialized = false;
static bool wifi_enabled = false;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static char got_ip_str[100] = "";
static bool ip_update_pending = false;
static lv_timer_t *ip_update_timer = NULL;
static esp_event_handler_instance_t wifi_event_instance = NULL;
static esp_event_handler_instance_t ip_event_instance = NULL;
static bool event_handlers_registered = false;
static int sta_retry_count = 0;
static bool sta_connection_failed = false;

// Forward declarations
static void ip_update_timer_cb(lv_timer_t *timer);
static void sta_failure_update_timer_cb(lv_timer_t *timer);
static void start_wifi_ap(void);
static void stop_wifi_ap(void);
static void start_wifi_sta(void);
static void stop_wifi_sta(void);

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected", MAC2STR(event->mac));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected", MAC2STR(event->mac));
    } else if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP started, starting web server...");
        start_webserver();
    } else if (event_id == WIFI_EVENT_STA_START) {
        sta_retry_count = 0;
        esp_wifi_connect();
        ESP_LOGI(TAG, "STA started, connecting...");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (sta_retry_count < MAX_STA_RETRY) {
            esp_wifi_connect();
            sta_retry_count++;
            ESP_LOGI(TAG, "STA disconnected, retry %d/%d", sta_retry_count, MAX_STA_RETRY);
        } else {
            ESP_LOGE(TAG, "STA connection failed after %d attempts", MAX_STA_RETRY);
            sta_connection_failed = true;
            
            // Stop WiFi to prevent interference with LCD
            esp_wifi_stop();
            esp_wifi_deinit();
            
            // Destroy the network interface
            if (sta_netif) {
                esp_netif_destroy(sta_netif);
                sta_netif = NULL;
            }
            
            wifi_enabled = false;
            wifi_initialized = false;
            event_handlers_registered = false;
            
            // Update UI via timer
            lv_timer_t *failure_timer = lv_timer_create(sta_failure_update_timer_cb, 100, NULL);
            lv_timer_set_repeat_count(failure_timer, 1);
        }
    }
}

// Timer callback to update UI from LVGL task
static void ip_update_timer_cb(lv_timer_t *timer)
{
    if (ip_update_pending) {
        char status_text[150];
        snprintf(status_text, sizeof(status_text), "WiFi STA: Connected (%s)", got_ip_str);
        lv_label_set_text(status_label, status_text);
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
        if (connect_btn) {
            lv_label_set_text(lv_obj_get_child(connect_btn, 0), "Disconnect");
        }
        ip_update_pending = false;
    }
    
    // Delete the timer after execution
    if (ip_update_timer) {
        lv_timer_delete(ip_update_timer);
        ip_update_timer = NULL;
    }
}

// Timer callback to update UI on connection failure
static void sta_failure_update_timer_cb(lv_timer_t *timer)
{
    if (sta_connection_failed) {
        lv_label_set_text(status_label, "WiFi STA: Connection failed (check SSID/password)");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        if (connect_btn) {
            lv_label_set_text(lv_obj_get_child(connect_btn, 0), "Connect");
        }
        sta_connection_failed = false;
    }
    
    // Delete the timer
    lv_timer_delete(timer);
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Reset retry counter on successful connection
        sta_retry_count = 0;
        sta_connection_failed = false;
        
        // Store IP info and schedule UI update in LVGL task
        snprintf(got_ip_str, sizeof(got_ip_str), "IP: " IPSTR, 
                 IP2STR(&event->ip_info.ip));
        ip_update_pending = true;
        
        // Create one-shot timer to update UI from LVGL task
        if (!ip_update_timer) {
            ip_update_timer = lv_timer_create(ip_update_timer_cb, 100, NULL);
            lv_timer_set_repeat_count(ip_update_timer, 1);
        }
        
        // Start web server
        start_webserver();
    }
}

// Load WiFi configuration from NVS
static void load_wifi_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        uint8_t mode = WIFI_MODE_AP;
        nvs_get_u8(nvs_handle, "mode", &mode);
        current_wifi_mode = (wifi_mode_t)mode;
        
        // Set UI mode based on WiFi mode
        current_ui_mode = (mode == WIFI_MODE_STA) ? WIFI_UI_MODE_STA : WIFI_UI_MODE_AP;
        
        size_t ssid_len = MAX_SSID_LEN;
        nvs_get_str(nvs_handle, "sta_ssid", sta_ssid, &ssid_len);
        
        size_t pass_len = MAX_PASS_LEN;
        nvs_get_str(nvs_handle, "sta_pass", sta_password, &pass_len);
        
        size_t ap_ssid_len = MAX_SSID_LEN;
        nvs_get_str(nvs_handle, "ap_ssid", ap_ssid, &ap_ssid_len);
        
        size_t ap_pass_len = MAX_PASS_LEN;
        nvs_get_str(nvs_handle, "ap_pass", ap_password, &ap_pass_len);
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Loaded WiFi config: mode=%d, STA_SSID=%s, AP_SSID=%s", current_wifi_mode, sta_ssid, ap_ssid);
    } else {
        ESP_LOGI(TAG, "No saved WiFi config, using defaults");
    }
}

// Save WiFi configuration to NVS
static void save_wifi_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_u8(nvs_handle, "mode", (uint8_t)current_wifi_mode);
        nvs_set_str(nvs_handle, "sta_ssid", sta_ssid);
        nvs_set_str(nvs_handle, "sta_pass", sta_password);
        nvs_set_str(nvs_handle, "ap_ssid", ap_ssid);
        nvs_set_str(nvs_handle, "ap_pass", ap_password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Saved WiFi config");
    }
}

static void start_wifi_ap(void)
{
    // Network infrastructure (NVS, netif, event loop) is already initialized in main.c
    if (!wifi_initialized) {
        wifi_initialized = true;
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Reduce WiFi memory usage to avoid PSRAM conflicts with LCD framebuffer
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 8;
    cfg.tx_buf_type = 1;  // Use static TX buffers
    cfg.static_tx_buf_num = 2;
    cfg.dynamic_tx_buf_num = 8;
    cfg.cache_tx_buf_num = 1;
    
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Disable WiFi power save to prevent CPU frequency changes
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // Register event handlers only once
    if (!event_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                             ESP_EVENT_ANY_ID,
                                                             &wifi_event_handler,
                                                             NULL,
                                                             &wifi_event_instance));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                             IP_EVENT_STA_GOT_IP,
                                                             &ip_event_handler,
                                                             NULL,
                                                             &ip_event_instance));
        event_handlers_registered = true;
    }
    
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, ap_ssid);
    strcpy((char *)wifi_config.ap.password, ap_password);
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    
    if (strlen(ap_password) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    
    // Stop audio playback before starting WiFi to prevent watchdog timeouts
    audio_player_stop();
    
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi AP starting. SSID:%s password:%s channel:%d",
             ap_ssid, ap_password, WIFI_AP_CHANNEL);
    
    // Webserver will be started by WIFI_EVENT_AP_START event
    
    wifi_enabled = true;
}

static void stop_wifi_ap(void)
{
    stop_webserver();
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // Destroy the network interface to allow recreation
    if (ap_netif) {
        esp_netif_destroy(ap_netif);
        ap_netif = NULL;
    }
    
    wifi_enabled = false;
    wifi_initialized = false;
    event_handlers_registered = false;
    
    ESP_LOGI(TAG, "WiFi AP stopped");
}

static void start_wifi_sta(void)
{
    if (!wifi_initialized) {
        wifi_initialized = true;
        sta_netif = esp_netif_create_default_wifi_sta();
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.static_rx_buf_num = 4;
        cfg.dynamic_rx_buf_num = 8;
        cfg.tx_buf_type = 1;
        cfg.static_tx_buf_num = 2;
        cfg.dynamic_tx_buf_num = 8;
        cfg.cache_tx_buf_num = 1;
        
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        
        // Register event handlers only once
        if (!event_handlers_registered) {
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                                 ESP_EVENT_ANY_ID,
                                                                 &wifi_event_handler,
                                                                 NULL,
                                                                 &wifi_event_instance));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                                 IP_EVENT_STA_GOT_IP,
                                                                 &ip_event_handler,
                                                                 NULL,
                                                                 &ip_event_instance));
            event_handlers_registered = true;
        }
    }
    
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, sta_ssid);
    strcpy((char *)wifi_config.sta.password, sta_password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Stop audio playback before starting WiFi to prevent watchdog timeouts
    audio_player_stop();
    
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi STA started. Connecting to SSID:%s", sta_ssid);
    
    // Update UI
    lv_label_set_text(status_label, "WiFi STA: Connecting...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFF00), 0);
    
    wifi_enabled = true;
}

static void stop_wifi_sta(void)
{
    stop_webserver();
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // Destroy the network interface to allow recreation
    if (sta_netif) {
        esp_netif_destroy(sta_netif);
        sta_netif = NULL;
    }
    
    wifi_enabled = false;
    wifi_initialized = false;
    event_handlers_registered = false;
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Update UI
    lv_label_set_text(status_label, "WiFi STA: Disconnected");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
    
    ESP_LOGI(TAG, "WiFi STA stopped");
}

static void mode_dropdown_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *dropdown = (lv_obj_t *)lv_event_get_target(e);
        uint16_t selected = lv_dropdown_get_selected(dropdown);
        
        if (selected == 0) {
            // AP mode
            current_ui_mode = WIFI_UI_MODE_AP;
            current_wifi_mode = WIFI_MODE_AP;
            lv_obj_add_flag(sta_container, LV_OBJ_FLAG_HIDDEN);
            if (ap_container) lv_obj_clear_flag(ap_container, LV_OBJ_FLAG_HIDDEN);
            if (ap_start_btn) lv_obj_clear_flag(ap_start_btn, LV_OBJ_FLAG_HIDDEN);
            
            // Stop STA if running, but don't auto-start AP
            if (wifi_enabled) {
                stop_wifi_sta();
            }
            
            // Update UI for AP mode - ensure button shows correct state
            lv_label_set_text(status_label, "WiFi AP: Stopped");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
            if (ap_start_btn) lv_label_set_text(lv_obj_get_child(ap_start_btn, 0), "Start AP");
            if (connect_btn) lv_label_set_text(lv_obj_get_child(connect_btn, 0), "Connect");
        } else {
            // STA mode
            current_ui_mode = WIFI_UI_MODE_STA;
            current_wifi_mode = WIFI_MODE_STA;
            lv_obj_clear_flag(sta_container, LV_OBJ_FLAG_HIDDEN);
            if (ap_container) lv_obj_add_flag(ap_container, LV_OBJ_FLAG_HIDDEN);
            if (ap_start_btn) lv_obj_add_flag(ap_start_btn, LV_OBJ_FLAG_HIDDEN);
            
            // Stop AP if running
            if (wifi_enabled) {
                stop_wifi_ap();
            }
            
            // Update UI for STA mode - ensure button shows correct state
            lv_label_set_text(status_label, "WiFi STA: Disconnected");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
            if (connect_btn) lv_label_set_text(lv_obj_get_child(connect_btn, 0), "Connect");
            if (ap_start_btn) lv_label_set_text(lv_obj_get_child(ap_start_btn, 0), "Start AP");
        }
        
        save_wifi_config();
    }
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_READY) {
        // User pressed OK/check button, hide keyboard
        if (keyboard) {
            lv_keyboard_set_textarea(keyboard, NULL);
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            
            // Restore original position of containers
            if (sta_container) {
                lv_obj_set_pos(sta_container, 20, 200);
            }
            if (ap_container) {
                lv_obj_set_pos(ap_container, 20, 150);
            }
        }
    }
}

static void sta_password_toggle_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        bool is_password_mode = lv_textarea_get_password_mode(password_textarea);
        lv_textarea_set_password_mode(password_textarea, !is_password_mode);
        
        // Update button label
        lv_obj_t *label = lv_obj_get_child(sta_password_toggle_btn, 0);
        lv_label_set_text(label, is_password_mode ? "Hide" : "Show");
    }
}

static void ap_password_toggle_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        bool is_password_mode = lv_textarea_get_password_mode(ap_password_textarea);
        lv_textarea_set_password_mode(ap_password_textarea, !is_password_mode);
        
        // Update button label
        lv_obj_t *label = lv_obj_get_child(ap_password_toggle_btn, 0);
        lv_label_set_text(label, is_password_mode ? "Hide" : "Show");
    }
}

static void textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *textarea = (lv_obj_t *)lv_event_get_target(e);
    
    if (code == LV_EVENT_FOCUSED) {
        // Show keyboard and link it to the focused textarea
        if (keyboard) {
            lv_keyboard_set_textarea(keyboard, textarea);
            lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            
            // Move the container to the top so textarea is visible above keyboard
            if (sta_container && !lv_obj_has_flag(sta_container, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_set_pos(sta_container, 20, 10);
            }
            if (ap_container && !lv_obj_has_flag(ap_container, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_set_pos(ap_container, 20, 10);
            }
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        // Hide keyboard when textarea loses focus
        if (keyboard) {
            lv_keyboard_set_textarea(keyboard, NULL);
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            
            // Restore original position of containers
            if (sta_container) {
                lv_obj_set_pos(sta_container, 20, 200);
            }
            if (ap_container) {
                lv_obj_set_pos(ap_container, 20, 150);
            }
        }
    }
}

static void ap_start_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        if (!wifi_enabled) {
            // Get AP SSID and password from textareas
            const char *ssid = lv_textarea_get_text(ap_ssid_textarea);
            const char *password = lv_textarea_get_text(ap_password_textarea);
            
            if (strlen(ssid) == 0) {
                lv_label_set_text(status_label, "Error: AP SSID cannot be empty");
                lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
                return;
            }
            
            // Save AP credentials
            strncpy(ap_ssid, ssid, MAX_SSID_LEN - 1);
            strncpy(ap_password, password, MAX_PASS_LEN - 1);
            save_wifi_config();
            
            // Start AP
            start_wifi_ap();
            lv_label_set_text(status_label, "WiFi AP: Active (IP: 192.168.4.1)");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
            lv_label_set_text(lv_obj_get_child(ap_start_btn, 0), "Stop AP");
        } else {
            // Stop AP
            stop_wifi_ap();
            lv_label_set_text(status_label, "WiFi AP: Stopped");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
            lv_label_set_text(lv_obj_get_child(ap_start_btn, 0), "Start AP");
        }
    }
}

static void connect_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        if (!wifi_enabled) {
            // Connect to WiFi
            const char *ssid = lv_textarea_get_text(ssid_textarea);
            const char *password = lv_textarea_get_text(password_textarea);
            
            if (strlen(ssid) == 0) {
                lv_label_set_text(status_label, "Error: SSID cannot be empty");
                lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
                return;
            }
            
            // Save credentials
            strncpy(sta_ssid, ssid, MAX_SSID_LEN - 1);
            strncpy(sta_password, password, MAX_PASS_LEN - 1);
            save_wifi_config();
            
            // Reset retry counter
            sta_retry_count = 0;
            sta_connection_failed = false;
            
            // Start connection
            start_wifi_sta();
            lv_label_set_text(lv_obj_get_child(connect_btn, 0), "Disconnect");
        } else {
            // Disconnect from WiFi
            stop_wifi_sta();
            lv_label_set_text(status_label, "WiFi STA: Disconnected");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
            lv_label_set_text(lv_obj_get_child(connect_btn, 0), "Connect");
        }
    }
}

static void wifi_config_gesture_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        
        if (dir == LV_DIR_RIGHT) {
            ESP_LOGI(TAG, "Swipe RIGHT detected, returning to file manager");
            wifi_config_hide();
        } else if (dir == LV_DIR_LEFT) {
            ESP_LOGI(TAG, "Swipe LEFT detected, showing button config");
            button_config_show();
        }
    }
}

void wifi_config_ui_init(lv_obj_t *parent)
{
    // Load saved WiFi config
    load_wifi_config();
    
    // Create WiFi config screen as an independent screen
    wifi_config_screen = lv_obj_create(NULL);
    lv_obj_set_size(wifi_config_screen, SUNTON_ESP32_LCD_WIDTH, SUNTON_ESP32_LCD_HEIGHT);
    lv_obj_set_style_bg_color(wifi_config_screen, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(wifi_config_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(wifi_config_screen, LV_SCROLLBAR_MODE_AUTO);
    
    // Add gesture event
    lv_obj_add_event_cb(wifi_config_screen, wifi_config_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    
    // Title
    lv_obj_t *title = lv_label_create(wifi_config_screen);
    lv_label_set_text(title, "WiFi Configuration");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    // Page indicators (5 circles)
    for (int i = 0; i < 5; i++) {
        lv_obj_t *circle = lv_obj_create(wifi_config_screen);
        lv_obj_set_size(circle, 12, 12);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(circle, 2, 0);
        lv_obj_set_style_border_color(circle, lv_color_hex(0x00FF00), 0);
        if (i == 2) {
            lv_obj_set_style_bg_color(circle, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);
        }
        lv_obj_align(circle, LV_ALIGN_TOP_RIGHT, -10 - (i * 18), 12);
    }    
    // Mode selection dropdown
    lv_obj_t *mode_label = lv_label_create(wifi_config_screen);
    lv_label_set_text(mode_label, "WiFi Mode:");
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(mode_label, 20, 60);
    
    mode_dropdown = lv_dropdown_create(wifi_config_screen);
    lv_dropdown_set_options(mode_dropdown, "Access Point (AP)\nStation (STA)");
    lv_obj_set_size(mode_dropdown, 300, 40);
    lv_obj_set_pos(mode_dropdown, 180, 55);
    lv_obj_set_style_text_font(mode_dropdown, &lv_font_montserrat_28, 0);
    lv_dropdown_set_selected(mode_dropdown, current_ui_mode == WIFI_UI_MODE_AP ? 0 : 1);
    lv_obj_add_event_cb(mode_dropdown, mode_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Status label
    status_label = lv_label_create(wifi_config_screen);
    lv_label_set_text(status_label, "WiFi: Inactive");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
    lv_obj_set_pos(status_label, 20, 110);
    
    // STA configuration container (hidden by default in AP mode)
    sta_container = lv_obj_create(wifi_config_screen);
    lv_obj_set_size(sta_container, 760, 300);
    lv_obj_set_pos(sta_container, 20, 150);
    lv_obj_set_style_bg_color(sta_container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(sta_container, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(sta_container, 2, 0);
    lv_obj_clear_flag(sta_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // SSID input
    lv_obj_t *ssid_label = lv_label_create(sta_container);
    lv_label_set_text(ssid_label, "Network SSID:");
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(ssid_label, 10, 10);
    
    ssid_textarea = lv_textarea_create(sta_container);
    lv_obj_set_size(ssid_textarea, 720, 50);
    lv_obj_set_pos(ssid_textarea, 10, 40);
    lv_obj_set_style_text_font(ssid_textarea, &lv_font_montserrat_28, 0);
    lv_textarea_set_placeholder_text(ssid_textarea, "Enter WiFi SSID");
    lv_textarea_set_one_line(ssid_textarea, true);
    lv_textarea_set_max_length(ssid_textarea, MAX_SSID_LEN - 1);
    lv_textarea_set_text(ssid_textarea, sta_ssid);
    lv_obj_add_event_cb(ssid_textarea, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ssid_textarea, textarea_event_cb, LV_EVENT_DEFOCUSED, NULL);
    
    // Password input
    lv_obj_t *pass_label = lv_label_create(sta_container);
    lv_label_set_text(pass_label, "Password:");
    lv_obj_set_style_text_font(pass_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(pass_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(pass_label, 10, 100);
    
    password_textarea = lv_textarea_create(sta_container);
    lv_obj_set_size(password_textarea, 720, 50);
    lv_obj_set_pos(password_textarea, 10, 130);
    lv_obj_set_style_text_font(password_textarea, &lv_font_montserrat_28, 0);
    lv_textarea_set_placeholder_text(password_textarea, "Enter password");
    lv_textarea_set_one_line(password_textarea, true);
    lv_textarea_set_max_length(password_textarea, MAX_PASS_LEN - 1);
    lv_textarea_set_password_mode(password_textarea, true);
    lv_textarea_set_text(password_textarea, sta_password);
    lv_obj_add_event_cb(password_textarea, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(password_textarea, textarea_event_cb, LV_EVENT_DEFOCUSED, NULL);
    
    // Password show/hide toggle button
    sta_password_toggle_btn = lv_btn_create(sta_container);
    lv_obj_set_size(sta_password_toggle_btn, 80, 50);
    lv_obj_set_pos(sta_password_toggle_btn, 650, 130);
    lv_obj_set_style_bg_color(sta_password_toggle_btn, lv_color_hex(0x555555), 0);
    lv_obj_add_event_cb(sta_password_toggle_btn, sta_password_toggle_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *sta_toggle_label = lv_label_create(sta_password_toggle_btn);
    lv_label_set_text(sta_toggle_label, "Show");
    lv_obj_set_style_text_font(sta_toggle_label, &lv_font_montserrat_20, 0);
    lv_obj_center(sta_toggle_label);
    
    // Connect button
    connect_btn = lv_btn_create(sta_container);
    lv_obj_set_size(connect_btn, 200, 50);
    lv_obj_set_pos(connect_btn, 270, 200);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x00AA00), 0);
    lv_obj_add_event_cb(connect_btn, connect_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *btn_label = lv_label_create(connect_btn);
    lv_label_set_text(btn_label, "Connect");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(btn_label);
    
    // Create keyboard for text input (initially hidden)
    keyboard = lv_keyboard_create(wifi_config_screen);
    lv_obj_set_size(keyboard, SUNTON_ESP32_LCD_WIDTH, SUNTON_ESP32_LCD_HEIGHT / 2);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    
    // AP configuration container (shown when in AP mode)
    ap_container = lv_obj_create(wifi_config_screen);
    lv_obj_set_size(ap_container, 760, 240);
    lv_obj_set_pos(ap_container, 20, 150);
    lv_obj_set_style_bg_color(ap_container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(ap_container, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(ap_container, 2, 0);
    lv_obj_clear_flag(ap_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // AP SSID input
    lv_obj_t *ap_ssid_label = lv_label_create(ap_container);
    lv_label_set_text(ap_ssid_label, "AP SSID:");
    lv_obj_set_style_text_font(ap_ssid_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ap_ssid_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(ap_ssid_label, 10, 10);
    
    ap_ssid_textarea = lv_textarea_create(ap_container);
    lv_obj_set_size(ap_ssid_textarea, 720, 50);
    lv_obj_set_pos(ap_ssid_textarea, 10, 40);
    lv_obj_set_style_text_font(ap_ssid_textarea, &lv_font_montserrat_28, 0);
    lv_textarea_set_placeholder_text(ap_ssid_textarea, "Enter AP SSID");
    lv_textarea_set_one_line(ap_ssid_textarea, true);
    lv_textarea_set_max_length(ap_ssid_textarea, MAX_SSID_LEN - 1);
    lv_textarea_set_text(ap_ssid_textarea, ap_ssid);
    lv_obj_add_event_cb(ap_ssid_textarea, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ap_ssid_textarea, textarea_event_cb, LV_EVENT_DEFOCUSED, NULL);
    
    // AP Password input
    lv_obj_t *ap_pass_label = lv_label_create(ap_container);
    lv_label_set_text(ap_pass_label, "AP Password:");
    lv_obj_set_style_text_font(ap_pass_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ap_pass_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(ap_pass_label, 10, 95);
    
    ap_password_textarea = lv_textarea_create(ap_container);
    lv_obj_set_size(ap_password_textarea, 720, 50);
    lv_obj_set_pos(ap_password_textarea, 10, 125);
    lv_obj_set_style_text_font(ap_password_textarea, &lv_font_montserrat_28, 0);
    lv_textarea_set_placeholder_text(ap_password_textarea, "Enter AP password");
    lv_textarea_set_one_line(ap_password_textarea, true);
    lv_textarea_set_max_length(ap_password_textarea, MAX_PASS_LEN - 1);
    lv_textarea_set_password_mode(ap_password_textarea, true);
    lv_textarea_set_text(ap_password_textarea, ap_password);
    lv_obj_add_event_cb(ap_password_textarea, textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ap_password_textarea, textarea_event_cb, LV_EVENT_DEFOCUSED, NULL);
    
    // Password show/hide toggle button
    ap_password_toggle_btn = lv_btn_create(ap_container);
    lv_obj_set_size(ap_password_toggle_btn, 80, 50);
    lv_obj_set_pos(ap_password_toggle_btn, 650, 125);
    lv_obj_set_style_bg_color(ap_password_toggle_btn, lv_color_hex(0x555555), 0);
    lv_obj_add_event_cb(ap_password_toggle_btn, ap_password_toggle_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *ap_toggle_label = lv_label_create(ap_password_toggle_btn);
    lv_label_set_text(ap_toggle_label, "Show");
    lv_obj_set_style_text_font(ap_toggle_label, &lv_font_montserrat_20, 0);
    lv_obj_center(ap_toggle_label);
    
    // AP Start/Stop button
    ap_start_btn = lv_btn_create(wifi_config_screen);
    lv_obj_set_size(ap_start_btn, 300, 50);
    lv_obj_set_pos(ap_start_btn, 250, 400);
    lv_obj_set_style_bg_color(ap_start_btn, lv_color_hex(0x00AA00), 0);
    lv_obj_add_event_cb(ap_start_btn, ap_start_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ap_btn_label = lv_label_create(ap_start_btn);
    lv_label_set_text(ap_btn_label, "Start AP");
    lv_obj_set_style_text_font(ap_btn_label, &lv_font_montserrat_28, 0);
    lv_obj_center(ap_btn_label);
    
    // Hide/show appropriate containers based on mode
    if (current_ui_mode == WIFI_UI_MODE_STA) {
        lv_obj_clear_flag(sta_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ap_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ap_start_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sta_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ap_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ap_start_btn, LV_OBJ_FLAG_HIDDEN);
        
        // Don't auto-start AP, just set UI to stopped state
        lv_label_set_text(status_label, "WiFi AP: Stopped");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
    }
    
    ESP_LOGI(TAG, "WiFi config UI initialized");
}

void wifi_config_show(void)
{
    if (wifi_config_screen) {
        // Reset password fields to hidden mode
        if (password_textarea) {
            lv_textarea_set_password_mode(password_textarea, true);
        }
        if (ap_password_textarea) {
            lv_textarea_set_password_mode(ap_password_textarea, true);
        }
        
        // Reset toggle button labels to "Show"
        if (sta_password_toggle_btn) {
            lv_obj_t *label = lv_obj_get_child(sta_password_toggle_btn, 0);
            if (label) {
                lv_label_set_text(label, "Show");
            }
        }
        if (ap_password_toggle_btn) {
            lv_obj_t *label = lv_obj_get_child(ap_password_toggle_btn, 0);
            if (label) {
                lv_label_set_text(label, "Show");
            }
        }
        
        lv_screen_load(wifi_config_screen);
        ESP_LOGI(TAG, "WiFi config shown");
    }
}

void wifi_config_hide(void)
{
    // Return to file manager
    file_manager_show();
    ESP_LOGI(TAG, "Returned to file manager");
}

lv_obj_t * wifi_config_get_screen(void)
{
    return wifi_config_screen;
}
