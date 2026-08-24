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
#include <math.h>
#include <ctype.h>
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

#include "disp_ota.h"
#include "crank_config.h"
#include "potis.h"
#include "song_settings.h"
#include "cJSON.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "web_server";

/* ── Configuration ─────────────────────────────────────────────────── */
#define AP_SSID        "MusicPlayer"
#define AP_PASS        "Crank!837"
#define AP_CHANNEL     6
#define AP_MAX_CONN    4
#define MOUNT_POINT    "/sdcard"
#define XFER_BUF_SIZE  16384   /* bytes per SD read/write chunk            */
#define MAX_FNAME_LEN    128   /* max accepted filename length (bytes)     */
#define MAX_BASENAME_LEN  44   /* max basename chars (excl. .wav extension) */
#define PLAYLISTS_CFG_PATH MOUNT_POINT "/playlists.json"

/* ── State ─────────────────────────────────────────────────────────── */
static rescan_cb_t           s_rescan_cb         = nullptr;
static web_song_settings_cb_t s_song_settings_cb  = nullptr;
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

static int ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static bool streq_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

static void trim_copy(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;

    const char *beg = src;
    while (*beg && isspace((unsigned char)*beg)) ++beg;

    const char *end = src + strlen(src);
    while (end > beg && isspace((unsigned char)end[-1])) --end;

    size_t n = (size_t)(end - beg);
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, beg, n);
    dst[n] = '\0';
}

static bool playlist_name_reserved(const char *name)
{
    return name && streq_ci(name, "All songs");
}

static bool playlist_array_contains_name_ci(const cJSON *arr, const char *name)
{
    if (!cJSON_IsArray(arr) || !name) return false;
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
        if (cJSON_IsString(nm) && nm->valuestring && streq_ci(nm->valuestring, name)) {
            return true;
        }
    }
    return false;
}

/* Update playlists.json song entries after a WAV rename.
 * Best-effort only: rename succeeds even if playlist sync fails. */
static void sync_playlists_song_rename(const char *old_name, const char *new_name)
{
    if (!old_name || !new_name || old_name[0] == '\0' || new_name[0] == '\0') return;
    if (streq_ci(old_name, new_name)) return;

    struct stat st = {};
    if (stat(PLAYLISTS_CFG_PATH, &st) != 0 || st.st_size <= 0 || st.st_size > 16384) return;

    FILE *f = fopen(PLAYLISTS_CFG_PATH, "r");
    if (!f) return;

    char *buf = (char *)malloc((size_t)st.st_size + 1u);
    if (!buf) {
        fclose(f);
        return;
    }

    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *pls = cJSON_GetObjectItemCaseSensitive(root, "playlists");
    if (!cJSON_IsArray(pls)) {
        cJSON_Delete(root);
        return;
    }

    bool changed = false;
    cJSON *pl = nullptr;
    cJSON_ArrayForEach(pl, pls) {
        if (!cJSON_IsObject(pl)) continue;
        cJSON *songs = cJSON_GetObjectItemCaseSensitive(pl, "songs");
        if (!cJSON_IsArray(songs)) continue;

        int song_count = cJSON_GetArraySize(songs);
        for (int i = 0; i < song_count; ++i) {
            cJSON *se = cJSON_GetArrayItem(songs, i);
            if (!cJSON_IsString(se) || !se->valuestring) continue;
            if (!streq_ci(se->valuestring, old_name)) continue;

            cJSON *rep = cJSON_CreateString(new_name);
            if (!rep) continue;
            cJSON_ReplaceItemInArray(songs, i, rep);
            changed = true;
        }
    }

    if (!changed) {
        cJSON_Delete(root);
        return;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    FILE *wf = fopen(PLAYLISTS_CFG_PATH, "w");
    if (!wf) {
        cJSON_free(json);
        ESP_LOGW(TAG, "Could not write playlists sync after rename");
        return;
    }
    fputs(json, wf);
    fclose(wf);
    cJSON_free(json);

    ESP_LOGI(TAG, "Playlists synced for rename: %s -> %s", old_name, new_name);
}

/* Check whether a song file is referenced in any playlist. */
static bool playlists_has_song_reference(const char *song_name,
                                         char       *playlist_name_out,
                                         size_t      playlist_name_out_len)
{
    if (playlist_name_out && playlist_name_out_len > 0) playlist_name_out[0] = '\0';
    if (!song_name || song_name[0] == '\0') return false;

    struct stat st = {};
    if (stat(PLAYLISTS_CFG_PATH, &st) != 0 || st.st_size <= 0 || st.st_size > 16384) return false;

    FILE *f = fopen(PLAYLISTS_CFG_PATH, "r");
    if (!f) return false;

    char *buf = (char *)malloc((size_t)st.st_size + 1u);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    bool found = false;
    cJSON *pls = cJSON_GetObjectItemCaseSensitive(root, "playlists");
    if (cJSON_IsArray(pls)) {
        cJSON *pl = nullptr;
        cJSON_ArrayForEach(pl, pls) {
            if (!cJSON_IsObject(pl)) continue;
            cJSON *songs = cJSON_GetObjectItemCaseSensitive(pl, "songs");
            if (!cJSON_IsArray(songs)) continue;

            cJSON *se = nullptr;
            cJSON_ArrayForEach(se, songs) {
                if (!cJSON_IsString(se) || !se->valuestring) continue;
                if (!streq_ci(se->valuestring, song_name)) continue;

                if (playlist_name_out && playlist_name_out_len > 0) {
                    cJSON *nm = cJSON_GetObjectItemCaseSensitive(pl, "name");
                    if (cJSON_IsString(nm) && nm->valuestring) {
                        strncpy(playlist_name_out, nm->valuestring, playlist_name_out_len - 1);
                        playlist_name_out[playlist_name_out_len - 1] = '\0';
                    }
                }
                found = true;
                break;
            }
            if (found) break;
        }
    }

    cJSON_Delete(root);
    return found;
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
extern const uint8_t index_html_start[]  asm("_binary_index_html_start");
extern const uint8_t index_html_end[]    asm("_binary_index_html_end");
extern const uint8_t update_html_start[] asm("_binary_update_html_start");
extern const uint8_t update_html_end[]   asm("_binary_update_html_end");

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

/* ── GET /update ────────────────────────────────────────────────────── */

static esp_err_t update_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    size_t len = (size_t)(update_html_end - update_html_start);
    return httpd_resp_send(req, (const char *)update_html_start, (ssize_t)len);
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
    int  n_total = 0, n_skipped_type = 0, n_skipped_ext = 0, n_sent = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        n_total++;
        ESP_LOGI(TAG, "readdir: '%s'  d_type=%u", entry->d_name, (unsigned)entry->d_type);
        /* Skip non-regular entries (d_type may be DT_UNKNOWN on FAT; use extension filter below) */
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) { n_skipped_type++; continue; }
        const char *name = entry->d_name;
        size_t      nlen = strlen(name);
        if (nlen < 5) { n_skipped_ext++; continue; }
        const char *ext = name + nlen - 4;
        if (!(ext[0] == '.' &&
              (ext[1] == 'w' || ext[1] == 'W') &&
              (ext[2] == 'a' || ext[2] == 'A') &&
              (ext[3] == 'v' || ext[3] == 'V'))) { n_skipped_ext++; continue; }

        char path[256];
        build_path(path, sizeof(path), name);
        struct stat st = {};
        long sz = (stat(path, &st) == 0) ? (long)st.st_size : 0L;
        ESP_LOGI(TAG, "  -> WAV: '%s'  size=%ld", name, sz);

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
        n_sent++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "files_get: total=%d skipped_type=%d skipped_ext=%d sent=%d",
             n_total, n_skipped_type, n_skipped_ext, n_sent);

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

    /* Trim basename to MAX_BASENAME_LEN characters (keep .wav extension). */
    {
        size_t total = strlen(fname);
        if (total > 4) {
            size_t base_len = total - 4; /* length without ".wav" */
            if (base_len > MAX_BASENAME_LEN) {
                memmove(fname + MAX_BASENAME_LEN, fname + base_len, 5); /* ".wav\0" */
            }
        }
    }

    char path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    build_path(path, sizeof(path), fname);

    /* Check if file already exists (unless ?replace=1 is set). */
    char replace_val[4] = {};
    bool do_replace = get_query_param(req, "replace", replace_val, sizeof(replace_val))
                      && replace_val[0] == '1';
    struct stat exist_st = {};
    if (!do_replace && stat(path, &exist_st) == 0) {
        /* Return 409 so the client can ask the user what to do. */
        char resp[160];
        snprintf(resp, sizeof(resp), "{\"exists\":true,\"name\":\"%s\"}", fname);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, resp);
    }

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

    sync_playlists_song_rename(old_name, new_name);

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

    char blocking_playlist[MAX_FNAME_LEN + 1] = {};
    if (playlists_has_song_reference(fname, blocking_playlist, sizeof(blocking_playlist))) {
        char msg[256];
        if (blocking_playlist[0] != '\0') {
            snprintf(msg, sizeof(msg), "File is used by playlist '%s'", blocking_playlist);
        } else {
            snprintf(msg, sizeof(msg), "File is used by at least one playlist");
        }
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, msg);
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

/* ── GET /api/playlists ─────────────────────────────────────────────── */

static esp_err_t playlists_get_handler(httpd_req_t *req)
{
    const char *fallback = "{\"active\":\"\",\"playlists\":[]}";
    struct stat st = {};
    if (stat(PLAYLISTS_CFG_PATH, &st) != 0 || st.st_size <= 0 || st.st_size > 16384) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
        return httpd_resp_sendstr(req, fallback);
    }

    FILE *f = fopen(PLAYLISTS_CFG_PATH, "r");
    if (!f) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
        return httpd_resp_sendstr(req, fallback);
    }

    char *buf = (char *)malloc((size_t)st.st_size + 1u);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

    if (!root) {
        return httpd_resp_sendstr(req, fallback);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return httpd_resp_sendstr(req, fallback);
    }

    esp_err_t ret = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ret;
}

/* ── POST /api/playlists ────────────────────────────────────────────── */

static esp_err_t playlists_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large or empty");
        return ESP_FAIL;
    }

    char *body = (char *)malloc((size_t)req->content_len + 1u);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int r = httpd_req_recv(req, body, (size_t)req->content_len);
    if (r <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[r] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *src_active = cJSON_GetObjectItemCaseSensitive(root, "active");
    cJSON *src_lists  = cJSON_GetObjectItemCaseSensitive(root, "playlists");
    if (!cJSON_IsArray(src_lists)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "playlists must be an array");
        return ESP_FAIL;
    }

    cJSON *out = cJSON_CreateObject();
    cJSON *out_lists = cJSON_CreateArray();
    if (!out || !out_lists) {
        cJSON_Delete(root);
        if (out) cJSON_Delete(out);
        if (out_lists) cJSON_Delete(out_lists);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int list_count = 0;
    cJSON *pl = nullptr;
    cJSON_ArrayForEach(pl, src_lists) {
        if (!cJSON_IsObject(pl)) continue;
        cJSON *nm = cJSON_GetObjectItemCaseSensitive(pl, "name");
        cJSON *sg = cJSON_GetObjectItemCaseSensitive(pl, "songs");
        if (!cJSON_IsString(nm) || !nm->valuestring) continue;
        if (!cJSON_IsArray(sg)) continue;

        char clean_name[MAX_FNAME_LEN + 1];
        trim_copy(nm->valuestring, clean_name, sizeof(clean_name));
        if (clean_name[0] == '\0') continue;
        if (playlist_name_reserved(clean_name)) continue;
        if (playlist_array_contains_name_ci(out_lists, clean_name)) continue;

        cJSON *one = cJSON_CreateObject();
        cJSON *songs = cJSON_CreateArray();
        if (!one || !songs) {
            if (one) cJSON_Delete(one);
            if (songs) cJSON_Delete(songs);
            cJSON_Delete(root);
            cJSON_Delete(out);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
            return ESP_FAIL;
        }

        cJSON_AddStringToObject(one, "name", clean_name);

        int song_count = 0;
        cJSON *se = nullptr;
        cJSON_ArrayForEach(se, sg) {
            if (song_count >= 256) break;
            if (!cJSON_IsString(se) || !se->valuestring) continue;
            if (!fname_valid(se->valuestring)) continue;
            cJSON_AddItemToArray(songs, cJSON_CreateString(se->valuestring));
            song_count++;
        }

        cJSON_AddItemToObject(one, "songs", songs);
        cJSON_AddItemToArray(out_lists, one);
        list_count++;
        if (list_count >= 32) break;
    }

    char active_name[MAX_FNAME_LEN + 1] = {0};
    if (cJSON_IsString(src_active) && src_active->valuestring) {
        trim_copy(src_active->valuestring, active_name, sizeof(active_name));
        if (playlist_name_reserved(active_name)) {
            active_name[0] = '\0';
        }
    }

    bool active_exists = (active_name[0] != '\0') && playlist_array_contains_name_ci(out_lists, active_name);

    cJSON_AddStringToObject(out, "active", active_exists ? active_name : "");
    cJSON_AddItemToObject(out, "playlists", out_lists);

    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    FILE *f = fopen(PLAYLISTS_CFG_PATH, "w");
    if (!f) {
        cJSON_free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write playlists file");
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    cJSON_free(json);

    if (s_rescan_cb) s_rescan_cb();
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_sendstr(req, "OK");
}
/* ── GET /api/crank_config ─────────────────────────────────────────── */

static esp_err_t crank_config_get_handler(httpd_req_t *req)
{
    char buf[520];
    snprintf(buf, sizeof(buf),
             "{\"ema_attack\":%.3f,\"ema_release\":%.3f,"
             "\"stop_thresh\":%.3f,\"start_thresh\":%.3f,"
             "\"release_ticks\":%u,\"vol_fade_step\":%u,"
             "\"dimmer_start_fade_ms\":%u,\"dimmer_stop_fade_ms\":%u,\"crank_dir\":%d,"
             "\"lo_bass_weight\":%.1f,\"lo_mid_weight\":%.1f,"
             "\"lo_decay_rate\":%.4f,\"lo_lookahead_s\":%.3f,"
             "\"pot_cal_lo\":%u,\"pot_cal_mid\":%u,\"pot_cal_hi\":%u}",
             (double)g_crank_cfg.ema_attack,
             (double)g_crank_cfg.ema_release,
             (double)g_crank_cfg.stop_thresh,
             (double)g_crank_cfg.start_thresh,
             (unsigned)g_crank_cfg.release_ticks,
             (unsigned)g_crank_cfg.vol_fade_step,
             (unsigned)g_crank_cfg.dimmer_start_fade_ms,
             (unsigned)g_crank_cfg.dimmer_stop_fade_ms,
             (int)g_crank_cfg.crank_dir,
             (double)g_crank_cfg.lo_bass_weight,
             (double)g_crank_cfg.lo_mid_weight,
             (double)g_crank_cfg.lo_decay_rate,
             (double)g_crank_cfg.lo_lookahead_s,
             (unsigned)g_crank_cfg.pot_cal_lo,
             (unsigned)g_crank_cfg.pot_cal_mid,
             (unsigned)g_crank_cfg.pot_cal_hi);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_sendstr(req, buf);
}

/* ── POST /api/crank_config  body: JSON object ───────────────────── */

static esp_err_t crank_config_post_handler(httpd_req_t *req)
{
    if (req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    char body[513] = {};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    /* Start from current values so partial updates are accepted */
    crank_config_t nc = g_crank_cfg;

    auto read_f = [](cJSON *obj, const char *key, float mn, float mx, float *dst) {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsNumber(it)) {
            float v = (float)it->valuedouble;
            if (v >= mn && v <= mx) *dst = v;
        }
    };
    auto read_u8 = [](cJSON *obj, const char *key, int mn, int mx, uint8_t *dst) {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsNumber(it)) {
            int v = (int)it->valuedouble;
            if (v >= mn && v <= mx) *dst = (uint8_t)v;
        }
    };
    auto read_u16 = [](cJSON *obj, const char *key, int mn, int mx, uint16_t *dst) {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsNumber(it)) {
            int v = (int)it->valuedouble;
            if (v >= mn && v <= mx) *dst = (uint16_t)v;
        }
    };

    read_f (root, "ema_attack",    0.005f, 0.500f, &nc.ema_attack);
    read_f (root, "ema_release",   0.500f, 2.000f, &nc.ema_release);
    read_f (root, "stop_thresh",   0.050f, 0.600f, &nc.stop_thresh);
    read_f (root, "start_thresh",  0.200f, 1.200f, &nc.start_thresh);
    read_u8(root, "release_ticks", 0, 10, &nc.release_ticks);
    read_u8(root, "vol_fade_step", 1, 10, &nc.vol_fade_step);
    read_u16(root, "dimmer_start_fade_ms", 0, 5000, &nc.dimmer_start_fade_ms);
    read_u16(root, "dimmer_stop_fade_ms",  0, 5000, &nc.dimmer_stop_fade_ms);
    {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "crank_dir");
        if (cJSON_IsNumber(it)) {
            int v = (int)it->valuedouble;
            if (v >= -1 && v <= 1) nc.crank_dir = (int8_t)v;
        }
    }
    read_f (root, "lo_bass_weight", 1.0f, 200.0f, &nc.lo_bass_weight);
    read_f (root, "lo_mid_weight",  0.0f,  50.0f, &nc.lo_mid_weight);
    read_f (root, "lo_decay_rate",  0.990f, 0.999f, &nc.lo_decay_rate);
    read_f (root, "lo_lookahead_s", -1.0f,  1.0f,   &nc.lo_lookahead_s);
    cJSON_Delete(root);

    if (nc.start_thresh <= nc.stop_thresh) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "start_thresh must be greater than stop_thresh");
        return ESP_FAIL;
    }

    g_crank_cfg = nc;
    crank_config_save();
    crank_config_apply();

    ESP_LOGI("web_server", "crank_config updated via web");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_sendstr(req, "OK");
}

/* ── POST /api/pot_cal ─────────────────────────────────────────────────
 * Guided 3-step calibration wizard.
 * Body: {"step":0|1|2}
 *   step 0 = pot at MINIMUM  → records raw_lo
 *   step 1 = pot at CENTER   → records raw_mid
 *   step 2 = pot at MAXIMUM  → records raw_hi, validates, commits & applies
 * Returns JSON: {"raw":NNN,"done":false} or {"raw":NNN,"done":true,...}
 * ──────────────────────────────────────────────────────────────────────── */

static uint16_t s_pot_cal_pending[3] = {559, 945, 3071};

static esp_err_t pot_cal_post_handler(httpd_req_t *req)
{
    if (req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }
    char body[65] = {};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *si = cJSON_GetObjectItemCaseSensitive(root, "step");
    if (!cJSON_IsNumber(si) || (int)si->valuedouble < 0 || (int)si->valuedouble > 2) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "step must be 0–2");
        return ESP_FAIL;
    }
    int step = (int)si->valuedouble;
    cJSON_Delete(root);

    uint16_t raw = potis_read_raw_avg();
    s_pot_cal_pending[step] = raw;

    char resp[120];
    if (step < 2) {
        snprintf(resp, sizeof(resp), "{\"raw\":%u,\"done\":false}", (unsigned)raw);
    } else {
        /* Validate: points must be strictly ascending with at least 100-count spread */
        uint16_t lo  = s_pot_cal_pending[0];
        uint16_t mid = s_pot_cal_pending[1];
        uint16_t hi  = s_pot_cal_pending[2];
        if (lo >= mid || mid >= hi || (mid - lo) < 100 || (hi - mid) < 100) {
            snprintf(resp, sizeof(resp),
                     "{\"raw\":%u,\"done\":false,"
                     "\"error\":\"Calibration points out of order or too close – retry\"}",
                     (unsigned)raw);
        } else {
            g_crank_cfg.pot_cal_lo  = lo;
            g_crank_cfg.pot_cal_mid = mid;
            g_crank_cfg.pot_cal_hi  = hi;
            crank_config_save();
            potis_set_cal(lo, mid, hi);
            snprintf(resp, sizeof(resp),
                     "{\"raw\":%u,\"done\":true,"
                     "\"lo\":%u,\"mid\":%u,\"hi\":%u}",
                     (unsigned)raw, (unsigned)lo, (unsigned)mid, (unsigned)hi);
            ESP_LOGI(TAG, "Pot calibration saved: lo=%u mid=%u hi=%u", lo, mid, hi);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_sendstr(req, resp);
}
/* ── POST /player_update ────────────────────────────────────────────── */

/**
 * Receive a raw firmware binary and flash it to the next OTA partition using
 * the ESP-IDF OTA API.  Streams text progress to the browser.  Restarts the
 * device automatically on success.
 *
 * Requires otadata + ota_0 partitions in the partition table.
 * Content-Length must be set by the client; max 4 MB accepted.
 */
static esp_err_t player_update_post_handler(httpd_req_t *req)
{
    int total = (int)req->content_len;
    ESP_LOGI(TAG, "player_update: content_len=%d", total);
    if (total <= 0 || total > 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "player_update: bad content_len=%d (must be 1–4 MB)", total);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Content-Length required and must be 1\xe2\x80\x93" "4 MB");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

#define PU_SEND(msg) httpd_resp_send_chunk(req, (msg), (ssize_t)strlen(msg))
    char log_buf[128];

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        PU_SEND("ERROR: No OTA partition found. Check partitions.csv.\n");
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    snprintf(log_buf, sizeof(log_buf),
             "OTA target: %s (0x%08lX, %lu kB)\n",
             part->label,
             (unsigned long)part->address,
             (unsigned long)(part->size / 1024u));
    PU_SEND(log_buf);

    if ((size_t)total > part->size) {
        snprintf(log_buf, sizeof(log_buf),
                 "ERROR: Image (%d bytes) exceeds partition size (%lu bytes).\n",
                 total, (unsigned long)part->size);
        PU_SEND(log_buf);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        snprintf(log_buf, sizeof(log_buf),
                 "ERROR: esp_ota_begin: %s\n", esp_err_to_name(err));
        PU_SEND(log_buf);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    snprintf(log_buf, sizeof(log_buf), "Receiving %d bytes...\n", total);
    PU_SEND(log_buf);

    int      received       = 0;
    int      last_report_kb = 0;
    esp_err_t recv_err      = ESP_OK;

    while (received < total) {
        int to_read = MIN((int)sizeof(s_xfer_buf), total - received);
        int r = httpd_req_recv(req, s_xfer_buf, (size_t)to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            snprintf(log_buf, sizeof(log_buf),
                     "ERROR: Receive failed after %d/%d bytes.\n", received, total);
            PU_SEND(log_buf);
            recv_err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota_handle, s_xfer_buf, (size_t)r);
        if (err != ESP_OK) {
            snprintf(log_buf, sizeof(log_buf),
                     "ERROR: esp_ota_write: %s\n", esp_err_to_name(err));
            PU_SEND(log_buf);
            recv_err = ESP_FAIL;
            break;
        }
        received += r;

        /* Report progress every 64 kB. */
        int current_kb = received / (64 * 1024);
        if (current_kb > last_report_kb) {
            last_report_kb = current_kb;
            snprintf(log_buf, sizeof(log_buf),
                     "  %d / %d kB (%d%%)\n",
                     received / 1024, total / 1024,
                     received * 100 / total);
            PU_SEND(log_buf);
        }
    }

    if (recv_err != ESP_OK) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        snprintf(log_buf, sizeof(log_buf),
                 "ERROR: esp_ota_end: %s\n", esp_err_to_name(err));
        PU_SEND(log_buf);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        snprintf(log_buf, sizeof(log_buf),
                 "ERROR: esp_ota_set_boot_partition: %s\n", esp_err_to_name(err));
        PU_SEND(log_buf);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    PU_SEND("SUCCESS: OTA write complete. Restarting in 2 s...\n");
    httpd_resp_send_chunk(req, nullptr, 0);  /* terminate chunked response */

    /* Allow the final HTTP chunk to flush before the restart. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

#undef PU_SEND
    return ESP_OK;  /* unreachable */
}

/* ── POST /disp_update?addr=<hex_or_dec> ────────────────────────────── */

/**
 * Receive a raw firmware binary, save it temporarily to the SD card, then
 * invoke disp_ota_flash() to reflash the display ESP32.
 *
 * The response is chunked plain-text so the browser can stream progress.
 * addr query parameter sets the target flash address (default 0x10000).
 * Content-Length must be set by the client; max 4 MB accepted.
 */
static esp_err_t disp_update_post_handler(httpd_req_t *req)
{
    int total = (int)req->content_len;
    if (total <= 0 || total > 4 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Content-Length required and must be 1–4 MB");
        return ESP_FAIL;
    }

    /* Parse optional flash address (hex or decimal). */
    uint32_t flash_addr = 0x10000u;
    char addr_str[24] = {};
    if (get_query_param(req, "addr", addr_str, sizeof(addr_str))) {
        long v = strtol(addr_str, nullptr, 0);
        if (v > 0 && v < 0x1000000L) flash_addr = (uint32_t)v;
    }

    /* Set up streaming plain-text response. */
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

#define DISP_FW_TMP_PATH  MOUNT_POINT "/disp_fw.bin"
#define SEND_LOG(msg) httpd_resp_send_chunk(req, (msg), (ssize_t)strlen(msg))

    char log_buf[120];
    snprintf(log_buf, sizeof(log_buf),
             "Receiving %d bytes -> " DISP_FW_TMP_PATH "\n", total);
    SEND_LOG(log_buf);

    /* Save binary to SD card. */
    FILE *f = fopen(DISP_FW_TMP_PATH, "wb");
    if (!f) {
        SEND_LOG("ERROR: Cannot create temp file on SD card.\n");
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    int received = 0;
    esp_err_t save_err = ESP_OK;
    while (received < total) {
        int to_read = MIN((int)sizeof(s_xfer_buf), total - received);
        int r = httpd_req_recv(req, s_xfer_buf, (size_t)to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            snprintf(log_buf, sizeof(log_buf),
                     "ERROR: Receive failed after %d/%d bytes.\n", received, total);
            SEND_LOG(log_buf);
            save_err = ESP_FAIL;
            break;
        }
        if ((int)fwrite(s_xfer_buf, 1, (size_t)r, f) != r) {
            SEND_LOG("ERROR: SD card write failed.\n");
            save_err = ESP_FAIL;
            break;
        }
        received += r;
    }
    fclose(f);

    if (save_err != ESP_OK) {
        remove(DISP_FW_TMP_PATH);
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_FAIL;
    }

    snprintf(log_buf, sizeof(log_buf),
             "Saved %d bytes. Flashing at 0x%08lX...\n",
             received, (unsigned long)flash_addr);
    SEND_LOG(log_buf);

    /* Flash the display – progress is streamed directly into req. */
    esp_err_t flash_ret = disp_ota_flash(DISP_FW_TMP_PATH, flash_addr, req);

    remove(DISP_FW_TMP_PATH);

    if (flash_ret == ESP_OK) {
        SEND_LOG("SUCCESS: Display reflashed and restarted.\n");
    } else {
        SEND_LOG("FAILED: See log above for details.\n");
    }

    httpd_resp_send_chunk(req, nullptr, 0);  /* terminate chunked response */

#undef SEND_LOG
#undef DISP_FW_TMP_PATH

    return (flash_ret == ESP_OK) ? ESP_OK : ESP_FAIL;
}

/* ── GET /api/song_settings?name=<file.wav> ────────────────────────── */

static esp_err_t song_settings_get_handler(httpd_req_t *req)
{
    char fname[MAX_FNAME_LEN + 1] = {};
    if (!get_query_param(req, "name", fname, sizeof(fname)) || !fname_valid(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing filename");
        return ESP_FAIL;
    }

    char wav_path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    build_path(wav_path, sizeof(wav_path), fname);

    song_settings_t s;
    song_settings_load(wav_path, &s);

    const char *end_action = s.loop ? "loop" : (s.autoplay_next ? "next" : "none");

    char buf[448];
    snprintf(buf, sizeof(buf),
             "{\"end_action\":\"%s\",\"loop\":%s,\"autoplay_next\":%s,\"fixed_speed_en\":%s,\"fixed_speed\":%.2f,"
             "\"pitch_influence\":%u,"
             "\"dimmer_max\":%u,\"dimmer_min\":%u,"
             "\"dimmer_rps_ref\":%.2f,\"dimmer_holdoff_s\":%u,\"dimmer_fadein_s\":%u,"
             "\"light_organ\":%s,\"downmix_mode\":%u,\"downmix_fade_s\":%u}",
             end_action,
             s.loop ? "true" : "false",
             s.autoplay_next ? "true" : "false",
             (s.fixed_speed > 0.0f) ? "true" : "false",
             (s.fixed_speed > 0.0f) ? (double)s.fixed_speed : 1.0,
             (unsigned)s.pitch_influence,
             (unsigned)s.dimmer_max,
             (unsigned)s.dimmer_min,
             (double)s.dimmer_rps_ref,
             (unsigned)s.dimmer_holdoff_s,
             (unsigned)s.dimmer_fadein_s,
             s.light_organ ? "true" : "false",
             (unsigned)s.downmix_mode,
             (unsigned)s.downmix_fade_s);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    return httpd_resp_sendstr(req, buf);
}

/* ── POST /api/song_settings?name=<file.wav> ────────────────────────── */

static esp_err_t song_settings_post_handler(httpd_req_t *req)
{
    char fname[MAX_FNAME_LEN + 1] = {};
    if (!get_query_param(req, "name", fname, sizeof(fname)) || !fname_valid(fname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing filename");
        return ESP_FAIL;
    }
    if (req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }
    char body[513] = {};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    bool    loop      = false;
    bool    autoplay_next = false;
    bool    fixed_en  = false;
    float   fixed_spd = 1.0f;
    uint8_t pitch     = 0;
    uint8_t d_max     = 100;
    uint8_t d_min     = 0;
    float   d_rps     = 1.4f;
    uint8_t d_hoff    = 0;
    uint8_t downmix_mode = 0;
    uint8_t downmix_fade_s = 1;

    cJSON *it;
    it = cJSON_GetObjectItemCaseSensitive(root, "end_action");
    if (cJSON_IsString(it) && it->valuestring) {
        if (strcmp(it->valuestring, "loop") == 0) {
            loop = true;
            autoplay_next = false;
        } else if (strcmp(it->valuestring, "next") == 0) {
            loop = false;
            autoplay_next = true;
        } else {
            loop = false;
            autoplay_next = false;
        }
    } else {
        it = cJSON_GetObjectItemCaseSensitive(root, "loop");
        if (cJSON_IsBool(it)) loop = cJSON_IsTrue(it);
        it = cJSON_GetObjectItemCaseSensitive(root, "autoplay_next");
        if (cJSON_IsBool(it)) autoplay_next = cJSON_IsTrue(it);
    }
    if (loop && autoplay_next) autoplay_next = false;
    it = cJSON_GetObjectItemCaseSensitive(root, "fixed_speed_en");
    if (cJSON_IsBool(it)) fixed_en = cJSON_IsTrue(it);
    it = cJSON_GetObjectItemCaseSensitive(root, "fixed_speed");
    if (cJSON_IsNumber(it)) { float v = (float)it->valuedouble; if (v >= 0.5f && v <= 2.0f) fixed_spd = v; }
    it = cJSON_GetObjectItemCaseSensitive(root, "pitch_influence");
    if (cJSON_IsNumber(it)) { int v = (int)it->valuedouble; pitch = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); }
    it = cJSON_GetObjectItemCaseSensitive(root, "dimmer_max");
    if (cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 100) d_max = (uint8_t)it->valueint;
    it = cJSON_GetObjectItemCaseSensitive(root, "dimmer_min");
    if (cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 100) d_min = (uint8_t)it->valueint;
    it = cJSON_GetObjectItemCaseSensitive(root, "dimmer_rps_ref");
    if (cJSON_IsNumber(it)) { float v = (float)it->valuedouble; if (v >= 0.1f && v <= 5.0f) d_rps = v; }
    it = cJSON_GetObjectItemCaseSensitive(root, "dimmer_holdoff_s");
    if (cJSON_IsNumber(it)) { int v = (int)it->valuedouble; d_hoff = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    uint8_t d_fadein = 0;
    it = cJSON_GetObjectItemCaseSensitive(root, "dimmer_fadein_s");
    if (cJSON_IsNumber(it)) { int v = (int)it->valuedouble; d_fadein = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    bool light_organ = false;
    it = cJSON_GetObjectItemCaseSensitive(root, "light_organ");
    if (cJSON_IsBool(it)) light_organ = cJSON_IsTrue(it);
    it = cJSON_GetObjectItemCaseSensitive(root, "downmix_mode");
    if (cJSON_IsNumber(it)) { int v = (int)it->valuedouble; downmix_mode = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v); }
    it = cJSON_GetObjectItemCaseSensitive(root, "downmix_fade_s");
    if (cJSON_IsNumber(it)) { int v = (int)it->valuedouble; downmix_fade_s = (uint8_t)(v < 0 ? 0 : v > 10 ? 10 : v); }
    cJSON_Delete(root);

    char wav_path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 2];
    build_path(wav_path, sizeof(wav_path), fname);
    char json_path[sizeof(MOUNT_POINT) + MAX_FNAME_LEN + 6];
    wav_to_json_path(wav_path, json_path, sizeof(json_path));

    bool dimmer_default = (d_max == 100 && d_min == 0 && fabsf(d_rps - 1.4f) <= 0.05f);
    if (!loop && !autoplay_next && !fixed_en && pitch == 0 && dimmer_default && d_hoff == 0 && d_fadein == 0 && !light_organ && downmix_mode == 0u) {
        if (downmix_fade_s == 1u) {
        remove(json_path);
        ESP_LOGI(TAG, "Song settings cleared via web for %s", fname);
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
        }
    }

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(out, "end_action", loop ? "loop" : (autoplay_next ? "next" : "none"));
    if (fixed_en)  cJSON_AddNumberToObject(out, "fixed_speed", (double)fixed_spd);
    if (pitch > 0) cJSON_AddNumberToObject(out, "pitch_influence", pitch);
    if (!dimmer_default) {
        cJSON_AddNumberToObject(out, "dimmer_max", d_max);
        cJSON_AddNumberToObject(out, "dimmer_min", d_min);
        cJSON_AddNumberToObject(out, "dimmer_rps_ref", (double)d_rps);
    }
    if (d_hoff > 0) cJSON_AddNumberToObject(out, "dimmer_holdoff_s", d_hoff);
    if (d_fadein > 0) cJSON_AddNumberToObject(out, "dimmer_fadein_s", d_fadein);
    if (light_organ) cJSON_AddBoolToObject(out, "light_organ", true);
    if (downmix_mode > 0u) cJSON_AddNumberToObject(out, "downmix_mode", downmix_mode);
    if (downmix_fade_s != 1u) cJSON_AddNumberToObject(out, "downmix_fade_s", downmix_fade_s);

    char *js = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!js) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    FILE *f = fopen(json_path, "w");
    if (f) { fputs(js, f); fclose(f); }
    cJSON_free(js);
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", json_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write settings file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Song settings saved via web for %s", fname);

    /* Live-apply to the running song if the callback is registered */
    if (s_song_settings_cb) {
        float fixed_speed_f = fixed_en ? fixed_spd : 0.0f;
        s_song_settings_cb(wav_path, loop, autoplay_next, fixed_speed_f, pitch,
                         d_max, d_min, d_rps, d_hoff, d_fadein, downmix_mode, downmix_fade_s);
        /* light_organ live-apply is handled via the same callback path: the
         * callback rereads the just-written JSON to pick up all new fields. */
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ── HTTP server ────────────────────────────────────────────────────── */

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 16384; /* OTA end/SHA256 verification needs more than default 4K */
    cfg.max_uri_handlers  = 16;
    cfg.recv_wait_timeout = 60;    /* seconds – generous for large OTA uploads */
    cfg.send_wait_timeout = 60;
    cfg.lru_purge_enable  = true;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return nullptr;
    }

    static const httpd_uri_t handlers[] = {
        { "/",                   HTTP_GET,    root_get_handler,              nullptr },
        { "/update",             HTTP_GET,    update_get_handler,            nullptr },
        { "/api/files",          HTTP_GET,    files_get_handler,             nullptr },
        { "/api/playlists",      HTTP_GET,    playlists_get_handler,         nullptr },
        { "/api/playlists",      HTTP_POST,   playlists_post_handler,        nullptr },
        { "/api/crank_config",   HTTP_GET,    crank_config_get_handler,      nullptr },
        { "/api/crank_config",   HTTP_POST,   crank_config_post_handler,     nullptr },
        { "/api/pot_cal",        HTTP_POST,   pot_cal_post_handler,          nullptr },
        { "/download",           HTTP_GET,    download_get_handler,          nullptr },
        { "/upload",             HTTP_POST,   upload_post_handler,           nullptr },
        { "/rename",             HTTP_POST,   rename_post_handler,           nullptr },
        { "/delete",             HTTP_DELETE, delete_handler,                nullptr },
        { "/disp_update",        HTTP_POST,   disp_update_post_handler,      nullptr },
        { "/player_update",      HTTP_POST,   player_update_post_handler,    nullptr },
        { "/api/song_settings",  HTTP_GET,    song_settings_get_handler,     nullptr },
        { "/api/song_settings",  HTTP_POST,   song_settings_post_handler,    nullptr },
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
    memcpy(ap_cfg.ap.password, AP_PASS, strlen(AP_PASS));
    ap_cfg.ap.channel        = AP_CHANNEL;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
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

void web_server_set_song_settings_callback(web_song_settings_cb_t cb)
{
    s_song_settings_cb = cb;
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

bool web_server_is_running(void)
{
    return s_running;
}
