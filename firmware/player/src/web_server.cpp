/**
 * @file web_server.cpp
 * @brief WiFi soft-AP + HTTP file-manager for the music player.
 *
 * Endpoints
 * ---------
 *   GET  /              → index.html (embedded)
 *   GET  /api/files     → JSON array [{name, size}, …] of WAV files
 *   GET  /download?name → stream WAV file to browser
 *   POST /upload?name   → receive raw file body, save to SD card
 *   POST /rename        → JSON body {old, new}
 *   DELETE /delete?name → remove file
 *
 * Security
 * --------
 *   All filenames are validated: no path separators, no "..", must end in
 *   .wav (case-insensitive), max 128 characters.
 *
 * WiFi AP
 * -------
 *   SSID: MusicPlayer  |  Password: musicplayer  |  IP: 192.168.4.1
 */

#include "web_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <dirent.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"

static const char *TAG = "web_server";

/* ── Configuration ─────────────────────────────────────────────────── */
#define AP_SSID        "MusicPlayer"
#define AP_CHANNEL     6
#define AP_MAX_CONN    4
#define MOUNT_POINT    "/sdcard"
#define XFER_BUF_SIZE  16384   /* bytes per SD read/write chunk            */
#define MAX_FNAME_LEN  128    /* max accepted filename length (bytes)     */

/* ── State ─────────────────────────────────────────────────────────── */
static rescan_cb_t    s_rescan_cb = nullptr;
static httpd_handle_t s_server    = nullptr;
static bool           s_running   = false;

/* Reusable transfer buffer – lives in BSS; HTTP server is single-task so
 * it is never accessed from two handlers simultaneously.                 */
static char s_xfer_buf[XFER_BUF_SIZE];

/* ── Helpers ────────────────────────────────────────────────────────── */

/** In-place URL-decode (percent-encoding + '+' → ' '). */
static void url_decode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i < maxlen - 1) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, nullptr, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/**
 * Validate a filename:
 *   - non-empty, ≤ MAX_FNAME_LEN
 *   - no '/', '\', or ".." sequences
 *   - must end in ".wav" (case-insensitive)
 */
static bool fname_valid(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len < 5 || len > MAX_FNAME_LEN) return false;
    if (strstr(name, "..") != nullptr) return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
    }
    /* .wav extension check */
    const char *e = name + len - 4;
    return (e[0] == '.' &&
            (e[1] == 'w' || e[1] == 'W') &&
            (e[2] == 'a' || e[2] == 'A') &&
            (e[3] == 'v' || e[3] == 'V'));
}

/** Build an absolute SD-card path from a bare filename. */
static void build_path(char *buf, size_t bufsz, const char *name)
{
    snprintf(buf, bufsz, "%s/%s", MOUNT_POINT, name);
}

/**
 * Derive the sidecar JSON settings path from a WAV path.
 * Replaces the trailing ".wav" extension with ".json".
 * Writes an empty string on error (path too short or buffer too small).
 */
static void wav_to_json_path(const char *wav_path, char *out, size_t bufsz)
{
    size_t len = strlen(wav_path);
    if (len < 4 || len + 2 >= bufsz) { out[0] = '\0'; return; }
    memcpy(out, wav_path, len - 4);
    memcpy(out + len - 4, ".json", 6); /* 5 chars + NUL */
}

/**
 * Extract and URL-decode a query parameter from a request.
 * Returns false if the key is absent or the result is empty.
 */
static bool get_query_param(httpd_req_t *req,
                            const char  *key,
                            char        *dst,
                            size_t       dstlen)
{
    /* httpd_req_get_url_query_str copies the raw query string */
    char raw[512] = {};
    if (httpd_req_get_url_query_str(req, raw, sizeof(raw)) != ESP_OK) return false;

    char encoded[256] = {};
    if (httpd_query_key_value(raw, key, encoded, sizeof(encoded)) != ESP_OK) return false;

    url_decode(dst, encoded, dstlen);
    return dst[0] != '\0';
}

/* ── GET / ──────────────────────────────────────────────────────────── */

/* Symbols injected by the linker from the EMBED_FILES mechanism. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

/* ── GET /api/files ─────────────────────────────────────────────────── */

static esp_err_t files_get_handler(httpd_req_t *req)
{
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "opendir(%s) failed: %d", MOUNT_POINT, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_sendstr_chunk(req, "[");

    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG) continue;
        const char *name = entry->d_name;
        size_t      nlen = strlen(name);
        if (nlen < 5) continue;
        const char *ext = name + nlen - 4;
        if (!(ext[0] == '.' &&
              (ext[1] == 'w' || ext[1] == 'W') &&
              (ext[2] == 'a' || ext[2] == 'A') &&
              (ext[3] == 'v' || ext[3] == 'V'))) continue;

        char path[256];
        build_path(path, sizeof(path), name);
        struct stat st = {};
        long sz = (stat(path, &st) == 0) ? (long)st.st_size : 0L;

        /* JSON-escape the filename (handles quotes and backslashes) */
        char esc[300] = {};
        const char *s = name;
        char       *d = esc;
        while (*s && (size_t)(d - esc) < sizeof(esc) - 3) {
            if (*s == '"' || *s == '\\') *d++ = '\\';
            *d++ = *s++;
        }

        char entry_buf[350];
        snprintf(entry_buf, sizeof(entry_buf),
                 "%s{\"name\":\"%s\",\"size\":%ld}",
                 first ? "" : ",", esc, sz);
        httpd_resp_sendstr_chunk(req, entry_buf);
        first = false;
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, nullptr); /* terminate chunked response */
    return ESP_OK;
}

/* ── GET /download?name=<file.wav> ──────────────────────────────────── */

static esp_err_t download_get_handler(httpd_req_t *req)
{
    char fname[MAX_FNAME_LEN + 1] = {};
    if (!get_query_param(req, "name", fname, sizeof(fname)) || !fname_valid(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing filename");
        return ESP_FAIL;
    }

    char path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    build_path(path, sizeof(path), fname);

    struct stat st = {};
    if (stat(path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: %d", path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "audio/wav");
    char cd[MAX_FNAME_LEN + 32];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", fname);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    char cl[24];
    snprintf(cl, sizeof(cl), "%ld", (long)st.st_size);
    httpd_resp_set_hdr(req, "Content-Length", cl);

    esp_err_t ret = ESP_OK;
    size_t    rd;
    while ((rd = fread(s_xfer_buf, 1, sizeof(s_xfer_buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, s_xfer_buf, (ssize_t)rd) != ESP_OK) {
            ESP_LOGW(TAG, "Download aborted: client disconnected");
            ret = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    if (ret == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
    return ret;
}

/* ── POST /upload?name=<file.wav> ────────────────────────────────────── */

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char fname[MAX_FNAME_LEN + 1] = {};
    if (!get_query_param(req, "name", fname, sizeof(fname)) || !fname_valid(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing filename");
        return ESP_FAIL;
    }

    char path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    build_path(path, sizeof(path), fname);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s, wb) failed: %d", path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_FAIL;
    }

    int       total    = (int)req->content_len;
    esp_err_t ret      = ESP_OK;
    int       written  = 0;

    int received = 0;
    while (received < total) {
        int to_read = MIN((int)sizeof(s_xfer_buf), total - received);
        int r = httpd_req_recv(req, s_xfer_buf, (size_t)to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            ESP_LOGE(TAG, "recv error %d after %d/%d bytes", r, received, total);
            ret = ESP_FAIL;
            break;
        }
        if ((int)fwrite(s_xfer_buf, 1, (size_t)r, f) != r) {
            ESP_LOGE(TAG, "SD write error at byte %d", received);
            ret = ESP_FAIL;
            break;
        }
        received += r;
    }
    written = total;
    fclose(f);

    if (ret != ESP_OK) {
        remove(path);   /* clean up partial file */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", fname, written);
    if (s_rescan_cb) s_rescan_cb();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ── POST /rename  body: {"old":"a.wav","new":"b.wav"} ─────────────── */

/** Extract a JSON string value for the given key from a compact JSON object.
 *  Handles only the simple case produced by JSON.stringify on the client. */
static bool json_extract(const char *json,
                         const char *key,
                         char       *out,
                         size_t      outlen)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outlen - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"');
}

static esp_err_t rename_post_handler(httpd_req_t *req)
{
    if (req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
        return ESP_FAIL;
    }

    char body[513] = {};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    char old_name[MAX_FNAME_LEN + 1] = {};
    char new_name[MAX_FNAME_LEN + 1] = {};
    if (!json_extract(body, "old", old_name, sizeof(old_name)) ||
        !json_extract(body, "new", new_name, sizeof(new_name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed JSON");
        return ESP_FAIL;
    }

    if (!fname_valid(old_name) || !fname_valid(new_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    char old_path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    char new_path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    build_path(old_path, sizeof(old_path), old_name);
    build_path(new_path, sizeof(new_path), new_name);

    /* Refuse to overwrite an existing file silently */
    struct stat st = {};
    if (stat(new_path, &st) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Destination already exists");
        return ESP_FAIL;
    }

    if (rename(old_path, new_path) != 0) {
        ESP_LOGE(TAG, "rename(%s, %s) failed: %d", old_path, new_path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_FAIL;
    }

    /* Also rename the optional JSON sidecar settings file if it exists. */
    char old_json[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 6];
    char new_json[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 6];
    wav_to_json_path(old_path, old_json, sizeof(old_json));
    wav_to_json_path(new_path, new_json, sizeof(new_json));
    if (old_json[0] != '\0' && new_json[0] != '\0') {
        struct stat jst = {};
        if (stat(old_json, &jst) == 0) {
            if (rename(old_json, new_json) == 0) {
                ESP_LOGI(TAG, "Renamed settings: %s -> %s", old_json, new_json);
            } else {
                ESP_LOGW(TAG, "Settings rename failed (%d) – WAV renamed OK", errno);
            }
        }
    }

    ESP_LOGI(TAG, "Renamed: %s -> %s", old_name, new_name);
    if (s_rescan_cb) s_rescan_cb();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ── DELETE /delete?name=<file.wav> ─────────────────────────────────── */

static esp_err_t delete_handler(httpd_req_t *req)
{
    char fname[MAX_FNAME_LEN + 1] = {};
    if (!get_query_param(req, "name", fname, sizeof(fname)) || !fname_valid(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing filename");
        return ESP_FAIL;
    }

    char path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    build_path(path, sizeof(path), fname);

    if (remove(path) != 0) {
        ESP_LOGE(TAG, "remove(%s) failed: %d", path, errno);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found or cannot delete");
        return ESP_FAIL;
    }

    /* Also delete the optional JSON sidecar settings file if it exists. */
    char json_path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 6];
    wav_to_json_path(path, json_path, sizeof(json_path));
    if (json_path[0] != '\0') {
        struct stat jst = {};
        if (stat(json_path, &jst) == 0) {
            if (remove(json_path) == 0) {
                ESP_LOGI(TAG, "Deleted settings: %s", json_path);
            } else {
                ESP_LOGW(TAG, "Settings delete failed (%d) – WAV deleted OK", errno);
            }
        }
    }

    ESP_LOGI(TAG, "Deleted: %s", fname);
    if (s_rescan_cb) s_rescan_cb();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ── HTTP server ────────────────────────────────────────────────────── */

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 8;
    cfg.recv_wait_timeout = 30;  /* seconds – per individual recv call   */
    cfg.send_wait_timeout = 30;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return nullptr;
    }

    static const httpd_uri_t handlers[] = {
        { "/",          HTTP_GET,    root_get_handler,     nullptr },
        { "/api/files", HTTP_GET,    files_get_handler,    nullptr },
        { "/download",  HTTP_GET,    download_get_handler, nullptr },
        { "/upload",    HTTP_POST,   upload_post_handler,  nullptr },
        { "/rename",    HTTP_POST,   rename_post_handler,  nullptr },
        { "/delete",    HTTP_DELETE, delete_handler,       nullptr },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        httpd_register_uri_handler(server, &handlers[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return server;
}

/* ── WiFi soft-AP ───────────────────────────────────────────────────── */

/**
 * One-time initialisation of the WiFi stack.
 * Sets up netif, event loop, and esp_wifi but does NOT start the AP.
 */
static void wifi_stack_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    /* esp_event_loop_create_default() returns ESP_ERR_INVALID_STATE if the
     * default loop already exists – that is harmless, ignore it. */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* Configure AP parameters once; they persist across start/stop cycles. */
    wifi_config_t ap_cfg = {};
    memcpy(ap_cfg.ap.ssid, AP_SSID, strlen(AP_SSID));
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(AP_SSID);
    ap_cfg.ap.channel        = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    ESP_LOGI(TAG, "WiFi stack initialised (AP not started)");
}

/* ── Public API ─────────────────────────────────────────────────────── */

void web_server_init(rescan_cb_t on_files_changed)
{
    s_rescan_cb = on_files_changed;
    wifi_stack_init();
    /* WiFi AP and HTTP server are NOT started here.
     * Call web_server_enable() to bring them up. */
    ESP_LOGI(TAG, "web_server_init done – WiFi disabled at boot");
}

void web_server_enable(void)
{
    if (s_running) {
        ESP_LOGD(TAG, "web_server_enable: already running");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    /* Disable all power-saving for maximum throughput. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi AP started  SSID=\"%s\"  IP=192.168.4.1", AP_SSID);

    s_server  = start_webserver();
    s_running = true;
    ESP_LOGI(TAG, "WiFi + HTTP server enabled");
}

void web_server_disable(void)
{
    if (!s_running) {
        ESP_LOGD(TAG, "web_server_disable: already stopped");
        return;
    }

    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    esp_wifi_stop();
    s_running = false;
    ESP_LOGI(TAG, "WiFi + HTTP server disabled");
}
