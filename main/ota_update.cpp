#include "ota_update.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "OTA_UPDATE";

// OTA state
static ota_status_t g_ota_status = OTA_STATUS_IDLE;
static char g_available_version[32] = {0};
static char g_error_message[256] = {0};
static ota_progress_callback_t g_progress_callback = NULL;

// Buffer for version check response
#define VERSION_BUFFER_SIZE 1024
static char version_buffer[VERSION_BUFFER_SIZE];
static int version_buffer_pos = 0;

extern "C" {

// HTTP event handler for version check
static esp_err_t version_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Copy data to buffer
                int copy_len = evt->data_len;
                if (version_buffer_pos + copy_len >= VERSION_BUFFER_SIZE) {
                    copy_len = VERSION_BUFFER_SIZE - version_buffer_pos - 1;
                }
                if (copy_len > 0) {
                    memcpy(version_buffer + version_buffer_pos, evt->data, copy_len);
                    version_buffer_pos += copy_len;
                    version_buffer[version_buffer_pos] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Download context for OTA
typedef struct {
    FILE *file;
    int downloaded;
    int total_size;
} download_context_t;

static download_context_t g_download_ctx;

// HTTP event handler for firmware download
static esp_err_t download_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                g_download_ctx.total_size = atoi(evt->header_value);
                ESP_LOGI(TAG, "Download size: %d bytes", g_download_ctx.total_size);
            }
            break;
            
        case HTTP_EVENT_ON_DATA:
            if (g_download_ctx.file && evt->data_len > 0) {
                size_t written = fwrite(evt->data, 1, evt->data_len, g_download_ctx.file);
                if (written != evt->data_len) {
                    ESP_LOGE(TAG, "SD card write error");
                    return ESP_FAIL;
                }
                g_download_ctx.downloaded += evt->data_len;
                
                // Update progress (0-50% for download)
                if (g_download_ctx.total_size > 0 && g_progress_callback) {
                    int progress = (g_download_ctx.downloaded * 50) / g_download_ctx.total_size;
                    char msg[80];
                    snprintf(msg, sizeof(msg), "DL %d/%d KB (Display flickers)", 
                            g_download_ctx.downloaded/1024, g_download_ctx.total_size/1024);
                    g_progress_callback(progress, msg);
                }
            }
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

bool ota_update_init(void)
{
    ESP_LOGI(TAG, "OTA Update Manager initialized");
    ESP_LOGI(TAG, "Current version: %s", FIRMWARE_VERSION);
    
    // Get current partition info
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "An OTA update has been performed. Validating...");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    
    ESP_LOGI(TAG, "Running partition: %s at offset 0x%lx", 
             running->label, running->address);
    
    return true;
}

bool ota_check_for_updates(ota_progress_callback_t callback)
{
    g_progress_callback = callback;
    g_ota_status = OTA_STATUS_CHECKING;
    
    if (g_progress_callback) {
        g_progress_callback(0, "Checking for updates...");
    }
    
    // Reset buffer
    version_buffer_pos = 0;
    memset(version_buffer, 0, VERSION_BUFFER_SIZE);
    
    // Configure HTTP client for version check
    esp_http_client_config_t config = {};
    config.url = GITHUB_VERSION_URL;
    config.event_handler = version_http_event_handler;
    config.timeout_ms = 10000;
    config.buffer_size = 4096;  // Increase buffer size for headers
    config.buffer_size_tx = 2048;  // Transmit buffer
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        snprintf(g_error_message, sizeof(g_error_message), "Failed to initialize HTTP client");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status = %d, content_length = %lld",
                status_code,
                esp_http_client_get_content_length(client));
        
        if (status_code == 200) {
            // Parse version from JSON manually (simple parser)
            // Looking for "version": "x.x.x"
            const char *version_key = "\"version\"";
            char *version_pos = strstr(version_buffer, version_key);
            if (version_pos) {
                // Find the opening quote after the colon
                char *value_start = strchr(version_pos + strlen(version_key), '\"');
                if (value_start) {
                    value_start++; // Skip opening quote
                    char *value_end = strchr(value_start, '\"');
                    if (value_end && (value_end - value_start) < (int)sizeof(g_available_version)) {
                        memcpy(g_available_version, value_start, value_end - value_start);
                        g_available_version[value_end - value_start] = '\0';
                        ESP_LOGI(TAG, "Available version: %s", g_available_version);
                        
                        // Compare versions
                        if (strcmp(g_available_version, FIRMWARE_VERSION) != 0) {
                            g_ota_status = OTA_STATUS_UPDATE_AVAILABLE;
                            if (g_progress_callback) {
                                char msg[128];
                                snprintf(msg, sizeof(msg), "Update available: v%s", g_available_version);
                                g_progress_callback(100, msg);
                            }
                            esp_http_client_cleanup(client);
                            return true;
                        } else {
                            g_ota_status = OTA_STATUS_NO_UPDATE;
                            if (g_progress_callback) {
                                g_progress_callback(100, "You have the latest version");
                            }
                            esp_http_client_cleanup(client);
                            return false;
                        }
                    }
                }
            }
            ESP_LOGE(TAG, "Failed to parse version from JSON");
            snprintf(g_error_message, sizeof(g_error_message), "Failed to parse version info");
        } else {
            ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
            snprintf(g_error_message, sizeof(g_error_message), "Server returned status %d", status_code);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        snprintf(g_error_message, sizeof(g_error_message), "Network error: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    g_ota_status = OTA_STATUS_ERROR;
    if (g_progress_callback) {
        g_progress_callback(0, g_error_message);
    }
    return false;
}

bool ota_perform_update(ota_progress_callback_t callback)
{
    g_progress_callback = callback;
    g_ota_status = OTA_STATUS_DOWNLOADING;
    
    if (g_progress_callback) {
        g_progress_callback(0, "DISPLAY WILL FLICKER - THAT'S NORMAL!");
    }
    
    ESP_LOGI(TAG, "Starting OTA update from: %s", GITHUB_RELEASE_URL);
    
    // Step 1: Download firmware to SD card using event handler
    const char *temp_file = "/sdcard/firmware_temp.bin";
    FILE *f = fopen(temp_file, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open SD card file for writing");
        snprintf(g_error_message, sizeof(g_error_message), "SD card write error");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    // Initialize download context
    g_download_ctx.file = f;
    g_download_ctx.downloaded = 0;
    g_download_ctx.total_size = 0;
    
    // Configure HTTP client with event handler for automatic redirect support
    esp_http_client_config_t config = {};
    config.url = GITHUB_RELEASE_URL;
    config.event_handler = download_http_event_handler;
    config.timeout_ms = 30000;
    config.buffer_size = 8192;
    config.buffer_size_tx = 2048;
    config.keep_alive_enable = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        fclose(f);
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "HTTP init failed");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    // Perform HTTP request - this handles redirects automatically
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    fclose(f);
    g_download_ctx.file = NULL;
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "Download failed: %s", esp_err_to_name(err));
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP Status = %d", status_code);
        esp_http_client_cleanup(client);
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "Server returned HTTP %d", status_code);
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    int content_length = g_download_ctx.total_size;
    esp_http_client_cleanup(client);
    
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid download size: %d", content_length);
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "Invalid download size");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    ESP_LOGI(TAG, "Download complete: %d bytes, starting flash from SD card", content_length);
    
    // Step 2: Flash from SD card
    g_ota_status = OTA_STATUS_INSTALLING;
    if (g_progress_callback) {
        g_progress_callback(50, "Flashing (Display flickers)");
    }
    
    f = fopen(temp_file, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open downloaded file");
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "Failed to open temp file");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        fclose(f);
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "No OTA partition");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx", update_partition->label, update_partition->address);
    
    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        fclose(f);
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "OTA begin failed");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    uint8_t *buffer = (uint8_t *)malloc(4096);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate flash buffer");
        esp_ota_abort(ota_handle);
        fclose(f);
        remove(temp_file);
        snprintf(g_error_message, sizeof(g_error_message), "Memory error");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    int written = 0;
    while (true) {
        size_t bytes_read = fread(buffer, 1, 4096, f);
        if (bytes_read == 0) break;
        
        err = esp_ota_write(ota_handle, buffer, bytes_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(buffer);
            esp_ota_abort(ota_handle);
            fclose(f);
            remove(temp_file);
            snprintf(g_error_message, sizeof(g_error_message), "Flash write failed");
            g_ota_status = OTA_STATUS_ERROR;
            if (g_progress_callback) {
                g_progress_callback(0, g_error_message);
            }
            return false;
        }
        
        written += bytes_read;
        
        // Update progress (50-100% for flashing)
        int progress = 50 + ((written * 50) / content_length);
        if (g_progress_callback) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Flash %d/%d KB (Display flickers)", written/1024, content_length/1024);
            g_progress_callback(progress, msg);
        }
    }
    
    free(buffer);
    fclose(f);
    remove(temp_file);
    
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        snprintf(g_error_message, sizeof(g_error_message), "OTA end failed: %s", esp_err_to_name(err));
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        snprintf(g_error_message, sizeof(g_error_message), "Set boot partition failed");
        g_ota_status = OTA_STATUS_ERROR;
        if (g_progress_callback) {
            g_progress_callback(0, g_error_message);
        }
        return false;
    }
    
    ESP_LOGI(TAG, "OTA update successful!");
    g_ota_status = OTA_STATUS_SUCCESS;
    if (g_progress_callback) {
        g_progress_callback(100, "Update complete! Rebooting...");
    }
    
    return true;
}

ota_status_t ota_get_status(void)
{
    return g_ota_status;
}

const char* ota_get_current_version(void)
{
    return FIRMWARE_VERSION;
}

const char* ota_get_available_version(void)
{
    return g_available_version[0] != '\0' ? g_available_version : NULL;
}

const char* ota_get_error_message(void)
{
    return g_error_message;
}

} // extern "C"
