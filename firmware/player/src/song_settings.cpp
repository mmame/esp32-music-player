/**
 * @file song_settings.cpp
 * @brief Load optional per-song JSON settings from the SD card.
 *
 * See song_settings.h for the supported JSON keys and file naming convention.
 */

#include "song_settings.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "song_cfg";

/* Maximum settings file size accepted (avoids heap exhaustion on corrupt SD). */
#define SETTINGS_MAX_BYTES  4096

void song_settings_load(const char *wav_path, song_settings_t *out)
{
    /* Safe defaults: no loop, follow crank speed, time-stretch mode, system dimmer. */
    out->loop             = false;
    out->fixed_speed      = 0.0f;
    out->pitch_influence  = 0u;
    out->dimmer_override  = false;
    out->dimmer_max       = 100u;
    out->dimmer_min       = 0u;
    out->dimmer_rps_ref   = 1.4f;
    out->dimmer_holdoff_s = 0u;

    if (!wav_path) return;

    /* Build JSON path: swap the trailing ".wav" for ".json". */
    size_t wav_len = strlen(wav_path);
    if (wav_len < 4 || strcasecmp(wav_path + wav_len - 4, ".wav") != 0) return;

    char json_path[256];
    if (wav_len >= sizeof(json_path)) return; /* path too long */
    memcpy(json_path, wav_path, wav_len - 4);
    memcpy(json_path + wav_len - 4, ".json", 6); /* includes NUL */

    /* Check existence and size before allocating a heap buffer. */
    struct stat st;
    if (stat(json_path, &st) != 0) return; /* no settings file – that's fine */

    if (st.st_size > SETTINGS_MAX_BYTES) {
        ESP_LOGW(TAG, "Settings file too large (%ld B), skipping: %s",
                 (long)st.st_size, json_path);
        return;
    }

    FILE *f = fopen(json_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s", json_path);
        return;
    }

    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) {
        ESP_LOGE(TAG, "OOM reading %s", json_path);
        fclose(f);
        return;
    }

    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGW(TAG, "JSON parse error in %s", json_path);
        return;
    }

    /* "loop": boolean */
    const cJSON *loop_item = cJSON_GetObjectItemCaseSensitive(root, "loop");
    if (cJSON_IsBool(loop_item)) {
        out->loop = cJSON_IsTrue(loop_item);
    }

    /* "fixed_speed": positive number (speed multiplier) */
    const cJSON *speed_item = cJSON_GetObjectItemCaseSensitive(root, "fixed_speed");
    if (cJSON_IsNumber(speed_item) && speed_item->valuedouble > 0.0) {
        out->fixed_speed = (float)speed_item->valuedouble;
    }

    /* "pitch_influence": 0-100; also accept legacy "pitch_follow" bool (→ 0 or 100) */
    const cJSON *pitch_item = cJSON_GetObjectItemCaseSensitive(root, "pitch_influence");
    if (cJSON_IsNumber(pitch_item)) {
        int v = pitch_item->valueint;
        out->pitch_influence = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    } else {
        const cJSON *pf = cJSON_GetObjectItemCaseSensitive(root, "pitch_follow");
        if (cJSON_IsTrue(pf)) out->pitch_influence = 100u;
    }

    /* "dimmer_override": boolean – use per-song dimmer values */
    const cJSON *dov_item = cJSON_GetObjectItemCaseSensitive(root, "dimmer_override");
    if (cJSON_IsBool(dov_item)) {
        out->dimmer_override = cJSON_IsTrue(dov_item);
    }

    /* "dimmer_max": integer 0-100 */
    const cJSON *dmax_item = cJSON_GetObjectItemCaseSensitive(root, "dimmer_max");
    if (cJSON_IsNumber(dmax_item) && dmax_item->valueint >= 0 && dmax_item->valueint <= 100) {
        out->dimmer_max = (uint8_t)dmax_item->valueint;
    }

    /* "dimmer_min": integer 0-100 */
    const cJSON *dmin_item = cJSON_GetObjectItemCaseSensitive(root, "dimmer_min");
    if (cJSON_IsNumber(dmin_item) && dmin_item->valueint >= 0 && dmin_item->valueint <= 100) {
        out->dimmer_min = (uint8_t)dmin_item->valueint;
    }

    /* "dimmer_rps_ref": positive float – RPS at full brightness */
    const cJSON *drps_item = cJSON_GetObjectItemCaseSensitive(root, "dimmer_rps_ref");
    if (cJSON_IsNumber(drps_item) && drps_item->valuedouble > 0.0) {
        out->dimmer_rps_ref = (float)drps_item->valuedouble;
    }

    /* "dimmer_holdoff_s": integer 0-255 seconds */
    const cJSON *dhld_item = cJSON_GetObjectItemCaseSensitive(root, "dimmer_holdoff_s");
    if (cJSON_IsNumber(dhld_item) && dhld_item->valueint >= 0 && dhld_item->valueint <= 255) {
        out->dimmer_holdoff_s = (uint8_t)dhld_item->valueint;
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Settings for '%s': loop=%s fixed_speed=%s(%.2f) pitch_influence=%u%% "
             "dimmer_override=%s max=%u min=%u rps_ref=%.1f holdoff=%us",
             json_path,
             out->loop ? "yes" : "no",
             out->fixed_speed > 0.0f ? "" : "off ",
             (double)out->fixed_speed,
             out->pitch_influence,
             out->dimmer_override ? "yes" : "no",
             out->dimmer_max, out->dimmer_min,
             (double)out->dimmer_rps_ref,
             out->dimmer_holdoff_s);
}
