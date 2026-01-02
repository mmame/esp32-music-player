#include "wifi_config_ui.h"
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

#define WIFI_AP_SSID "ESP32-FileManager"
#define WIFI_AP_PASS "12345678"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4
#define MOUNT_POINT "/sdcard"
#define NVS_NAMESPACE "wifi_config"
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64

// WiFi mode selection
typedef enum {
    WIFI_UI_MODE_AP,
    WIFI_UI_MODE_STA
} wifi_ui_mode_t;

// UI elements
static lv_obj_t *wifi_config_screen = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *ip_label = NULL;
static lv_obj_t *mode_dropdown = NULL;
static lv_obj_t *sta_container = NULL;
static lv_obj_t *ssid_textarea = NULL;
static lv_obj_t *password_textarea = NULL;
static lv_obj_t *connect_btn = NULL;
static lv_obj_t *keyboard = NULL;

// WiFi state
static wifi_ui_mode_t current_ui_mode = WIFI_UI_MODE_AP;
static wifi_mode_t current_wifi_mode = WIFI_MODE_AP;
static char sta_ssid[MAX_SSID_LEN] = "";
static char sta_password[MAX_PASS_LEN] = "";
static bool wifi_initialized = false;
static bool wifi_enabled = false;
static httpd_handle_t server = NULL;

// Forward declarations
static void start_wifi_ap(void);
static void stop_wifi_ap(void);
static void start_wifi_sta(void);
static void stop_wifi_sta(void);
static esp_err_t start_webserver(void);
static void stop_webserver(void);

// HTTP handlers
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    
    // Send HTML in chunks to avoid buffer overflow
    httpd_resp_sendstr_chunk(req, 
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>ESP32 File Manager</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; margin: 20px; background: #1a1a1a; color: #fff; }"
        "h1 { color: #00ff00; }"
        ".container { max-width: 800px; margin: 0 auto; }"
        ".file-list { background: #2a2a2a; padding: 15px; border-radius: 5px; margin: 20px 0; }"
        ".file-item { padding: 10px; border-bottom: 1px solid #3a3a3a; display: flex; justify-content: space-between; align-items: center; }"
        ".file-item:hover { background: #3a3a3a; }"
        ".file-name { flex-grow: 1; cursor: pointer; }"
        ".folder { color: #00aaff; font-weight: bold; }"
        ".file { color: #cccccc; }"
        ".file-size { color: #888; margin: 0 10px; }"
        ".btn-delete { background: #aa0000; color: white; border: none; padding: 5px 15px; cursor: pointer; border-radius: 3px; margin-left: 5px; }"
        ".btn-delete:hover { background: #cc0000; }"
        ".btn-rename { background: #0066aa; color: white; border: none; padding: 5px 15px; cursor: pointer; border-radius: 3px; margin-left: 5px; }"
        ".btn-rename:hover { background: #0088cc; }"
        ".upload-area { border: 2px dashed #00ff00; padding: 30px; text-align: center; margin: 20px 0; border-radius: 5px; cursor: pointer; }"
        ".upload-area.dragover { background: #2a4a2a; }"
        ".btn-upload { background: #00aa00; color: white; border: none; padding: 10px 20px; cursor: pointer; border-radius: 5px; margin: 10px 5px; }"
        ".btn-upload:hover { background: #00cc00; }"
        ".btn-back { background: #333; color: white; border: none; padding: 10px 20px; cursor: pointer; border-radius: 5px; margin: 10px 0; }"
        ".btn-back:hover { background: #555; }"
        "input[type='file'] { display: none; }"
        ".current-path { color: #888; margin: 10px 0; }"
        ".progress-container { background: #2a2a2a; padding: 15px; border-radius: 5px; margin: 20px 0; display: none; }"
        ".progress-bar { width: 100%; height: 30px; background: #333; border-radius: 5px; overflow: hidden; margin: 10px 0; position: relative; }"
        ".progress-fill { height: 100%; background: #00aa00; text-align: center; line-height: 30px; color: white; min-width: 30px; }"
        ".upload-status { color: #ccc; margin: 5px 0; font-size: 14px; }"
        ".upload-bytes { color: #888; margin: 5px 0; font-size: 12px; }"
        "</style>"
        "</head><body>"
        "<div class='container'>"
        "<h1>ESP32 File Manager</h1>"
        "<div class='current-path' id='currentPath'>/sdcard</div>"
        "<button class='btn-back' id='btnBack' style='display:none;' onclick='goUp()'>⬆ Up</button>"
        "<div class='progress-container' id='progressContainer'>"
        "<div class='upload-status' id='uploadStatus'>Uploading...</div>"
        "<div class='upload-bytes' id='uploadBytes'></div>"
        "<div class='progress-bar'><div class='progress-fill' id='progressFill'>0%</div></div>"
        "</div>"
        "<div class='upload-area' id='dropArea'>"
        "<p>Drag & Drop files here or click to upload</p>"
        "<input type='file' id='fileInput' multiple>"
        "<button class='btn-upload' onclick='document.getElementById(\"fileInput\").click()'>Choose Files</button>"
        "</div>"
        "<div class='file-list' id='fileList'>Loading...</div>"
        "</div>"
        "<script>\n"
        "let currentPath = '/sdcard';\n"
        "const dropArea = document.getElementById('dropArea');\n"
        "const fileInput = document.getElementById('fileInput');\n"
        "\n"
        "// Prevent default drag behaviors\n"
        "['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {\n"
        "  dropArea.addEventListener(eventName, e => { e.preventDefault(); e.stopPropagation(); });\n"
        "});\n"
        "\n"
        "// Highlight drop area when item is dragged over it\n"
        "['dragenter', 'dragover'].forEach(eventName => {\n"
        "  dropArea.addEventListener(eventName, () => dropArea.classList.add('dragover'));\n"
        "});\n"
        "['dragleave', 'drop'].forEach(eventName => {\n"
        "  dropArea.addEventListener(eventName, () => dropArea.classList.remove('dragover'));\n"
        "});\n"
        "\n"
        "// Handle dropped files\n"
        "dropArea.addEventListener('drop', e => {\n"
        "  const files = e.dataTransfer.files;\n"
        "  uploadFiles(files);\n"
        "});\n"
        "\n"
        "// Handle file input change\n"
        "fileInput.addEventListener('change', e => {\n"
        "  uploadFiles(e.target.files);\n"
        "});\n"
        "\n"
        "function uploadFiles(files) {\n"
        "  console.log('Uploading', files.length, 'files to:', currentPath);\n"
        "  const fileArray = Array.from(files);\n"
        "  let completed = 0;\n"
        "  const total = fileArray.length;\n"
        "  \n"
        "  document.getElementById('progressContainer').style.display = 'block';\n"
        "  const statusEl = document.getElementById('uploadStatus');\n"
        "  const bytesEl = document.getElementById('uploadBytes');\n"
        "  const progressFill = document.getElementById('progressFill');\n"
        "  \n"
        "  function uploadFile(file, index) {\n"
        "    return new Promise((resolve, reject) => {\n"
        "      const formData = new FormData();\n"
        "      formData.append('file', file);\n"
        "      formData.append('path', currentPath);\n"
        "      \n"
        "      const xhr = new XMLHttpRequest();\n"
        "      \n"
        "      xhr.upload.addEventListener('progress', (e) => {\n"
        "        if (e.lengthComputable) {\n"
        "          const fileProgress = (e.loaded / e.total) * 100;\n"
        "          const overallProgress = ((completed + (e.loaded / e.total)) / total) * 100;\n"
        "          progressFill.style.width = overallProgress + '%';\n"
        "          progressFill.textContent = Math.round(overallProgress) + '%';\n"
        "          bytesEl.textContent = formatBytes(e.loaded) + ' / ' + formatBytes(e.total);\n"
        "        }\n"
        "      });\n"
        "      \n"
        "      xhr.addEventListener('load', () => {\n"
        "        if (xhr.status >= 200 && xhr.status < 300) {\n"
        "          console.log('Upload complete:', file.name);\n"
        "          resolve(xhr.responseText);\n"
        "        } else {\n"
        "          reject(xhr.responseText || 'Upload failed');\n"
        "        }\n"
        "      });\n"
        "      \n"
        "      xhr.addEventListener('error', () => reject('Network error'));\n"
        "      \n"
        "      xhr.open('POST', '/upload');\n"
        "      statusEl.textContent = `Uploading ${index + 1}/${total}: ${file.name}`;\n"
        "      xhr.send(formData);\n"
        "    });\n"
        "  }\n"
        "  \n"
        "  async function uploadSequentially() {\n"
        "    for (let i = 0; i < fileArray.length; i++) {\n"
        "      try {\n"
        "        await uploadFile(fileArray[i], i);\n"
        "        completed++;\n"
        "      } catch (err) {\n"
        "        console.error('Upload error:', err);\n"
        "        alert('Upload failed for ' + fileArray[i].name + ': ' + err);\n"
        "        statusEl.textContent = 'Upload failed: ' + fileArray[i].name;\n"
        "        statusEl.style.color = '#ff0000';\n"
        "        setTimeout(() => {\n"
        "          document.getElementById('progressContainer').style.display = 'none';\n"
        "          statusEl.style.color = '#ccc';\n"
        "        }, 3000);\n"
        "        return;\n"
        "      }\n"
        "    }\n"
        "    \n"
        "    statusEl.textContent = `Upload complete: ${total} file(s)`;\n"
        "    bytesEl.textContent = '';\n"
        "    setTimeout(() => {\n"
        "      document.getElementById('progressContainer').style.display = 'none';\n"
        "      progressFill.style.width = '0%';\n"
        "      progressFill.textContent = '0%';\n"
        "    }, 2000);\n"
        "    loadFiles();\n"
        "  }\n"
        "  \n"
        "  uploadSequentially();\n"
        "  fileInput.value = '';\n"
        "}\n"
        "\n"
        "function loadFiles() {\n"
        "  console.log('Loading files from:', currentPath);\n"
        "  fetch('/list?path=' + encodeURIComponent(currentPath))\n"
        "    .then(r => {\n"
        "      console.log('Response status:', r.status);\n"
        "      if (!r.ok) throw new Error('HTTP error ' + r.status);\n"
        "      return r.json();\n"
        "    })\n"
        "    .then(data => {\n"
        "      console.log('Received data:', data);\n"
        "      document.getElementById('currentPath').textContent = currentPath;\n"
        "      document.getElementById('btnBack').style.display = currentPath === '/sdcard' ? 'none' : 'block';\n"
        "      let html = '';\n"
        "      data.files.forEach(f => {\n"
        "        const isDir = f.type === 'dir';\n"
        "        const size = isDir ? '' : `<span class='file-size'>${formatBytes(f.size)}</span>`;\n"
        "        const nameClass = isDir ? 'folder' : 'file';\n"
        "        const onclick = isDir ? `onclick='openFolder(\"${f.name}\")'` : '';\n"
        "        html += `<div class='file-item'>`;\n"
        "        html += `<span class='file-name ${nameClass}' ${onclick}>${isDir ? '📁 ' : '📄 '}${f.name}</span>`;\n"
        "        html += size;\n"
        "        if (!isDir) html += `<button class='btn-delete' onclick='deleteFile(\"${f.name}\")'>Delete</button>`;\n"
        "        if (!isDir) html += `<button class='btn-rename' onclick='renameFile(\"${f.name}\")'>Rename</button>`;\n"
        "        html += `</div>`;\n"
        "      });\n"
        "      document.getElementById('fileList').innerHTML = html || '<p>No files</p>';\n"
        "    })\n"
        "    .catch(err => {\n"
        "      console.error('Error loading files:', err);\n"
        "      document.getElementById('fileList').innerHTML = '<p style=\"color:red;\">Error: ' + err.message + '</p>';\n"
        "    });\n"
        "}\n"
        "\n"
        "function openFolder(name) {\n"
        "  currentPath = currentPath + '/' + name;\n"
        "  loadFiles();\n"
        "}\n"
        "\n"
        "function goUp() {\n"
        "  const lastSlash = currentPath.lastIndexOf('/');\n"
        "  if (lastSlash > 0) {\n"
        "    currentPath = currentPath.substring(0, lastSlash);\n"
        "    loadFiles();\n"
        "  }\n"
        "}\n"
        "\n"
        "function deleteFile(name) {\n"
        "  if (confirm('Delete ' + name + '?')) {\n"
        "    fetch('/delete?path=' + encodeURIComponent(currentPath + '/' + name), { method: 'DELETE' })\n"
        "      .then(r => r.text())\n"
        "      .then(data => { console.log(data); loadFiles(); });\n"
        "  }\n"
        "}\n"
        "\n"
        "function renameFile(name) {\n"
        "  const newName = prompt('Rename ' + name + ' to:', name);\n"
        "  if (newName && newName !== name) {\n"
        "    fetch('/rename', {\n"
        "      method: 'POST',\n"
        "      headers: { 'Content-Type': 'application/json' },\n"
        "      body: JSON.stringify({ oldPath: currentPath + '/' + name, newName: newName })\n"
        "    })\n"
        "    .then(r => r.text())\n"
        "    .then(data => { console.log(data); loadFiles(); })\n"
        "    .catch(err => alert('Rename failed: ' + err));\n"
        "  }\n"
        "}\n"
        "\n"
        "function formatBytes(bytes) {\n"
        "  if (bytes < 1024) return bytes + ' B';\n"
        "  else if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';\n"
        "  else return (bytes / 1048576).toFixed(1) + ' MB';\n"
        "}\n"
        "\n"
        "// Load files on page load\n"
        "window.addEventListener('DOMContentLoaded', function() {\n"
        "  console.log('Page loaded, fetching files...');\n"
        "  loadFiles();\n"
        "});\n"
        "</script>"
        "</body></html>");
    
    // End chunked response
    httpd_resp_sendstr_chunk(req, NULL);
    ESP_LOGI(TAG, "HTML page sent successfully");
    return ESP_OK;
}

// URL decode helper function
static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

// Structure for sorting files in web browser
typedef struct {
    char name[256];
    bool is_dir;
    long size;
} web_file_entry_t;

static esp_err_t list_get_handler(httpd_req_t *req)
{
    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    char path[256] = MOUNT_POINT;
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                // URL decode the path
                url_decode(path, param);
            }
        }
        free(buf);
    }
    
    ESP_LOGI(TAG, "Listing directory: %s", path);
    
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open directory");
        return ESP_FAIL;
    }
    
    // Collect all files first for sorting
    web_file_entry_t *file_list = (web_file_entry_t *)malloc(256 * sizeof(web_file_entry_t));
    if (!file_list) {
        closedir(dir);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    
    int file_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && file_count < 256) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
        
        struct stat st;
        bool is_dir = false;
        long size = 0;
        
        if (stat(filepath, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = st.st_size;
        }
        
        strncpy(file_list[file_count].name, entry->d_name, sizeof(file_list[file_count].name) - 1);
        file_list[file_count].name[sizeof(file_list[file_count].name) - 1] = '\0';
        file_list[file_count].is_dir = is_dir;
        file_list[file_count].size = size;
        file_count++;
    }
    closedir(dir);
    
    // Sort files alphabetically (same as audio player and file manager)
    if (file_count > 1) {
        for (int i = 0; i < file_count - 1; i++) {
            for (int j = i + 1; j < file_count; j++) {
                // Compare directories first (dirs before files), then alphabetically
                bool swap = false;
                if (file_list[i].is_dir && !file_list[j].is_dir) {
                    swap = false;  // Keep directories first
                } else if (!file_list[i].is_dir && file_list[j].is_dir) {
                    swap = true;  // Move directory before file
                } else {
                    // Both are dirs or both are files, sort alphabetically
                    if (strcasecmp(file_list[i].name, file_list[j].name) > 0) {
                        swap = true;
                    }
                }
                
                if (swap) {
                    web_file_entry_t temp = file_list[i];
                    file_list[i] = file_list[j];
                    file_list[j] = temp;
                }
            }
        }
    }
    
    // Send JSON response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"files\":[");
    
    for (int i = 0; i < file_count; i++) {
        if (i > 0) {
            httpd_resp_sendstr_chunk(req, ",");
        }
        
        char json_item[1024];
        snprintf(json_item, sizeof(json_item), 
                 "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld}",
                 file_list[i].name, 
                 file_list[i].is_dir ? "dir" : "file", 
                 file_list[i].size);
        httpd_resp_sendstr_chunk(req, json_item);
    }
    
    free(file_list);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char boundary[128];
    char filepath[512] = MOUNT_POINT;
    char filename[256] = {0};
    
    // Get boundary from Content-Type
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Content-Type") + 1;
    if (hdr_len > 1) {
        char *content_type = (char *)malloc(hdr_len);
        httpd_req_get_hdr_value_str(req, "Content-Type", content_type, hdr_len);
        char *boundary_start = strstr(content_type, "boundary=");
        if (boundary_start) {
            snprintf(boundary, sizeof(boundary), "--%s", boundary_start + 9);
        }
        free(content_type);
    }
    
    // Simplified upload handler - expects multipart/form-data
    // This is a basic implementation; a production system would need more robust parsing
    
    char buf[4096];  // Larger buffer for better upload performance
    int remaining = req->content_len;
    bool found_file = false;
    FILE *f = NULL;
    
    ESP_LOGI(TAG, "Upload started, content length: %d bytes", req->content_len);
    
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            break;
        }
        
        // Very simple parsing - look for filename and file content
        if (!found_file) {
            char *filename_start = strstr(buf, "filename=\"");
            if (filename_start) {
                filename_start += 10;
                char *filename_end = strchr(filename_start, '"');
                if (filename_end) {
                    size_t name_len = filename_end - filename_start;
                    strncpy(filename, filename_start, (name_len < sizeof(filename) - 1) ? name_len : sizeof(filename) - 1);
                    
                    // Find the actual file content (after double CRLF)
                    char *content_start = strstr(buf, "\r\n\r\n");
                    if (content_start) {
                        content_start += 4;
                        snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, filename);
                        
                        // Check if file already exists
                        FILE *test = fopen(filepath, "r");
                        if (test) {
                            fclose(test);
                            ESP_LOGW(TAG, "File already exists: %s", filepath);
                            
                            // Consume remaining data to prevent connection hang
                            while (remaining > 0) {
                                int discard_len = httpd_req_recv(req, buf, (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf));
                                if (discard_len <= 0) break;
                                remaining -= discard_len;
                            }
                            
                            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exists. Please delete it first or rename the file.");
                            return ESP_OK;
                        }
                        
                        f = fopen(filepath, "wb");
                        found_file = true;
                        
                        // Write the first chunk
                        int content_len = recv_len - (content_start - buf);
                        if (f && content_len > 0) {
                            fwrite(content_start, 1, content_len, f);
                        }
                    }
                }
            }
        } else if (f) {
            // Check for boundary (end of file)
            if (strstr(buf, boundary)) {
                // Find where boundary starts and only write before it
                char *boundary_pos = strstr(buf, boundary);
                int write_len = boundary_pos - buf - 2; // -2 for CRLF before boundary
                if (write_len > 0) {
                    fwrite(buf, 1, write_len, f);
                }
                break;
            } else {
                fwrite(buf, 1, recv_len, f);
            }
        }
        
        remaining -= recv_len;
    }
    
    if (f) {
        fclose(f);
        ESP_LOGI(TAG, "File uploaded: %s", filepath);
        
        // Stop playback and rescan audio files
        audio_player_stop();
        audio_player_scan_wav_files();
        
        httpd_resp_sendstr(req, "File uploaded successfully");
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }
    
    return ESP_OK;
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char filepath[512];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[512];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                // URL decode the filepath
                url_decode(filepath, param);
                
                if (remove(filepath) == 0) {
                    ESP_LOGI(TAG, "File deleted: %s", filepath);
                    
                    // Stop playback and rescan audio files
                    audio_player_stop();
                    audio_player_scan_wav_files();
                    
                    httpd_resp_sendstr(req, "File deleted");
                } else {
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
                }
            }
        }
        free(buf);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path parameter");
    }
    
    return ESP_OK;
}

static esp_err_t rename_handler(httpd_req_t *req)
{
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    // Parse JSON: {"oldPath": "/sdcard/file.mp3", "newName": "newfile.mp3"}
    char oldPath[256] = {0};
    char newName[128] = {0};
    
    char *oldPathStart = strstr(content, "\"oldPath\":\"");
    char *newNameStart = strstr(content, "\"newName\":\"");
    
    if (oldPathStart && newNameStart) {
        oldPathStart += 11; // skip "oldPath":"
        char *oldPathEnd = strchr(oldPathStart, '"');
        if (oldPathEnd) {
            int len = oldPathEnd - oldPathStart;
            if (len < sizeof(oldPath)) {
                strncpy(oldPath, oldPathStart, len);
                oldPath[len] = '\0';
            }
        }
        
        newNameStart += 11; // skip "newName":"
        char *newNameEnd = strchr(newNameStart, '"');
        if (newNameEnd) {
            int len = newNameEnd - newNameStart;
            if (len < sizeof(newName)) {
                strncpy(newName, newNameStart, len);
                newName[len] = '\0';
            }
        }
    }
    
    if (strlen(oldPath) == 0 || strlen(newName) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // Build new path (same directory as old file)
    char newPath[256];
    strncpy(newPath, oldPath, sizeof(newPath) - 1);
    newPath[sizeof(newPath) - 1] = '\0';
    
    char *lastSlash = strrchr(newPath, '/');
    if (lastSlash) {
        *(lastSlash + 1) = '\0'; // Keep path up to last slash
        strncat(newPath, newName, sizeof(newPath) - strlen(newPath) - 1);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }
    
    // Perform rename
    if (rename(oldPath, newPath) == 0) {
        ESP_LOGI(TAG, "File renamed: %s -> %s", oldPath, newPath);
        
        // Stop playback and rescan audio files
        audio_player_stop();
        audio_player_scan_wav_files();
        
        httpd_resp_sendstr(req, "File renamed");
    } else {
        ESP_LOGE(TAG, "Rename failed: %s -> %s", oldPath, newPath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
    }
    
    return ESP_OK;
}

static esp_err_t start_webserver(void)
{
    // Don't start if server is already running
    if (server != NULL) {
        ESP_LOGI(TAG, "HTTP server already running");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.recv_wait_timeout = 60;  // 60 seconds for large uploads
    config.send_wait_timeout = 60;
    config.lru_purge_enable = true;
    
    ESP_LOGI(TAG, "Starting HTTP server");
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);
        
        httpd_uri_t list_uri = {
            .uri = "/list",
            .method = HTTP_GET,
            .handler = list_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &list_uri);
        
        httpd_uri_t upload_uri = {
            .uri = "/upload",
            .method = HTTP_POST,
            .handler = upload_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &upload_uri);
        
        httpd_uri_t delete_uri = {
            .uri = "/delete",
            .method = HTTP_DELETE,
            .handler = delete_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &delete_uri);
        
        httpd_uri_t rename_uri = {
            .uri = "/rename",
            .method = HTTP_POST,
            .handler = rename_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &rename_uri);
        
        ESP_LOGI(TAG, "HTTP server started successfully");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}

static void stop_webserver(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

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
        esp_wifi_connect();
        ESP_LOGI(TAG, "STA started, connecting...");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "STA disconnected, retrying...");
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        char ip_str[100];
        snprintf(ip_str, sizeof(ip_str), "WiFi STA: Connected");
        lv_label_set_text(status_label, ip_str);
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
        
        char ip_info[100];
        snprintf(ip_info, sizeof(ip_info), "IP: " IPSTR, 
                 IP2STR(&event->ip_info.ip));
        lv_label_set_text(ip_label, ip_info);
        
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
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Loaded WiFi config: mode=%d, SSID=%s", current_wifi_mode, sta_ssid);
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
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Saved WiFi config");
    }
}

static void start_wifi_ap(void)
{
    // Network infrastructure (NVS, netif, event loop) is already initialized in main.c
    // Just mark as initialized and create the AP interface
    wifi_initialized = true;
    
    esp_netif_create_default_wifi_ap();
    
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
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                         ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler,
                                                         NULL,
                                                         NULL));
    
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, WIFI_AP_SSID);
    strcpy((char *)wifi_config.ap.password, WIFI_AP_PASS);
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    
    if (strlen(WIFI_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    
    // Stop audio playback before starting WiFi to prevent watchdog timeouts
    audio_player_stop();
    
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi AP starting. SSID:%s password:%s channel:%d",
             WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL);
    
    // Webserver will be started by WIFI_EVENT_AP_START event
    
    wifi_enabled = true;
}

static void stop_wifi_ap(void)
{
    stop_webserver();
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    wifi_enabled = false;
    ESP_LOGI(TAG, "WiFi AP stopped");
}

static void start_wifi_sta(void)
{
    if (!wifi_initialized) {
        wifi_initialized = true;
        esp_netif_create_default_wifi_sta();
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.static_rx_buf_num = 4;
        cfg.dynamic_rx_buf_num = 8;
        cfg.tx_buf_type = 1;
        cfg.static_tx_buf_num = 2;
        cfg.dynamic_tx_buf_num = 8;
        cfg.cache_tx_buf_num = 1;
        
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                             ESP_EVENT_ANY_ID,
                                                             &wifi_event_handler,
                                                             NULL,
                                                             NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                             IP_EVENT_STA_GOT_IP,
                                                             &ip_event_handler,
                                                             NULL,
                                                             NULL));
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
    
    wifi_enabled = false;
    wifi_initialized = false;
    
    // Update UI
    lv_label_set_text(status_label, "WiFi STA: Disconnected");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
    lv_label_set_text(ip_label, "Not connected");
    
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
            
            // Stop STA if running and start AP
            if (wifi_enabled) {
                stop_wifi_sta();
                start_wifi_ap();
            }
        } else {
            // STA mode
            current_ui_mode = WIFI_UI_MODE_STA;
            current_wifi_mode = WIFI_MODE_STA;
            lv_obj_clear_flag(sta_container, LV_OBJ_FLAG_HIDDEN);
            
            // Stop AP if running
            if (wifi_enabled) {
                stop_wifi_ap();
            }
        }
        
        save_wifi_config();
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
            
            // Move the STA container to the top so textarea is visible above keyboard
            if (sta_container) {
                lv_obj_set_pos(sta_container, 20, 10);
            }
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        // Hide keyboard when textarea loses focus
        if (keyboard) {
            lv_keyboard_set_textarea(keyboard, NULL);
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            
            // Restore original position of STA container
            if (sta_container) {
                lv_obj_set_pos(sta_container, 20, 200);
            }
        }
    }
}

static void connect_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        // Get SSID and password from textareas
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
        
        // Connect
        if (wifi_enabled) {
            stop_wifi_sta();
        }
        start_wifi_sta();
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
    
    // IP label
    ip_label = lv_label_create(wifi_config_screen);
    lv_label_set_text(ip_label, "Not connected");
    lv_obj_set_style_text_font(ip_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ip_label, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(ip_label, 20, 140);
    lv_label_set_long_mode(ip_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ip_label, 760);
    
    // STA configuration container (hidden by default in AP mode)
    sta_container = lv_obj_create(wifi_config_screen);
    lv_obj_set_size(sta_container, 760, 300);
    lv_obj_set_pos(sta_container, 20, 200);
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
    
    // AP info (shown when in AP mode)
    lv_obj_t *ap_info = lv_label_create(wifi_config_screen);
    lv_label_set_text(ap_info, 
        "AP Mode Info:\n"
        "SSID: " WIFI_AP_SSID "\n"
        "Password: " WIFI_AP_PASS "\n"
        "IP: 192.168.4.1\n\n"
        "Access web interface at:\nhttp://192.168.4.1");
    lv_obj_set_style_text_font(ap_info, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ap_info, lv_color_hex(0x00AAFF), 0);
    lv_obj_set_pos(ap_info, 20, 200);
    
    // Hide/show appropriate containers based on mode
    if (current_ui_mode == WIFI_UI_MODE_STA) {
        lv_obj_clear_flag(sta_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ap_info, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sta_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ap_info, LV_OBJ_FLAG_HIDDEN);
        
        // Start AP automatically
        start_wifi_ap();
        lv_label_set_text(status_label, "WiFi AP: Active");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
        lv_label_set_text(ip_label, "IP: 192.168.4.1");
    }
    
    ESP_LOGI(TAG, "WiFi config UI initialized");
}

void wifi_config_show(void)
{
    if (wifi_config_screen) {
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
