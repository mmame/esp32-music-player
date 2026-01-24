#include "webserver.h"
#include "audio_player_ui.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <cctype>
#include "ff.h"

static const char *TAG = "Webserver";

#define MOUNT_POINT "/sdcard"

// Global webserver handle
httpd_handle_t server = NULL;

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
        ".file-size { color: #888; margin: 0 10px; }"        ".btn-download { background: transparent; color: white; border: 1px solid #555; padding: 5px 10px; cursor: pointer; border-radius: 3px; margin-left: 5px; font-size: 20px; line-height: 1; }\n"
        ".btn-download:hover { background: #3a3a3a; border-color: #00ff00; }\n"        ".btn-delete { background: #aa0000; color: white; border: none; padding: 5px 15px; cursor: pointer; border-radius: 3px; margin-left: 5px; }"
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
        "<div class='current-path' id='currentPath'>/sdcard</div>"        "<div class='current-path' id='diskSpace' style='font-size: 14px; color: #999; margin-top: 5px;'>Loading...</div>\n"        "<button class='btn-back' id='btnBack' style='display:none;' onclick='goUp()'>⬆ Up</button>"
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
        "      // Update disk space display\n"
        "      if (data.disk) {\n"
        "        document.getElementById('diskSpace').textContent = \n"
        "          `Used: ${data.disk.used} MB / ${data.disk.total} MB (Free: ${data.disk.free} MB)`;\n"
        "      }\n"
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
        "        if (!isDir) html += `<button class='btn-download' onclick='downloadFile(\"${f.name}\")'>💾</button>`;\n"
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
        "function downloadFile(name) {\n"
        "  const url = '/download?path=' + encodeURIComponent(currentPath + '/' + name);\n"
        "  window.location.href = url;\n"
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
    
    // Get disk space information using FatFs API
    FATFS *fs;
    DWORD free_clusters;
    long total_mb = 0, free_mb = 0, used_mb = 0;
    
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res == FR_OK) {
        // Calculate total and free space
        uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
        uint64_t free_sectors = free_clusters * fs->csize;
        
        // Convert to MB (sector size is 512 bytes)
        total_mb = (long)((total_sectors * 512) / (1024 * 1024));
        free_mb = (long)((free_sectors * 512) / (1024 * 1024));
        used_mb = total_mb - free_mb;
    }
    
    // Send JSON response
    httpd_resp_set_type(req, "application/json");
    
    // Send disk space info first
    char disk_info[256];
    snprintf(disk_info, sizeof(disk_info), 
             "{\"disk\":{\"total\":%ld,\"used\":%ld,\"free\":%ld},\"files\":[",
             total_mb, used_mb, free_mb);
    httpd_resp_sendstr_chunk(req, disk_info);
    
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

static esp_err_t download_handler(httpd_req_t *req)
{
    char filepath[512];
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                url_decode(filepath, param);
            }
        }
        free(buf);
    }
    
    ESP_LOGI(TAG, "Downloading file: %s", filepath);
    
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    
    // Extract filename from path
    const char *filename = strrchr(filepath, '/');
    if (filename) {
        filename++; // Skip the '/'
    } else {
        filename = filepath;
    }
    
    // Set Content-Disposition header to force download (limit filename length to avoid truncation)
    char content_disp[384];
    char safe_filename[256];
    strncpy(safe_filename, filename, sizeof(safe_filename) - 1);
    safe_filename[sizeof(safe_filename) - 1] = '\0';
    snprintf(content_disp, sizeof(content_disp), "attachment; filename=\"%s\"", safe_filename);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disp);
    
    // Determine content type
    const char *ext = strrchr(filename, '.');
    if (ext) {
        if (strcasecmp(ext, ".mp3") == 0) {
            httpd_resp_set_type(req, "audio/mpeg");
        } else if (strcasecmp(ext, ".wav") == 0) {
            httpd_resp_set_type(req, "audio/wav");
        } else if (strcasecmp(ext, ".txt") == 0) {
            httpd_resp_set_type(req, "text/plain");
        } else {
            httpd_resp_set_type(req, "application/octet-stream");
        }
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }
    
    // Stream file in chunks
    char *chunk = (char *)malloc(4096);
    if (!chunk) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, 4096, file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            ESP_LOGE(TAG, "File sending failed");
            free(chunk);
            fclose(file);
            return ESP_FAIL;
        }
    }
    
    free(chunk);
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    
    ESP_LOGI(TAG, "File download completed: %s", filename);
    return ESP_OK;
}

esp_err_t start_webserver(void)
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
        
        httpd_uri_t download_uri = {
            .uri = "/download",
            .method = HTTP_GET,
            .handler = download_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &download_uri);
        
        ESP_LOGI(TAG, "HTTP server started successfully");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}

void stop_webserver(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
