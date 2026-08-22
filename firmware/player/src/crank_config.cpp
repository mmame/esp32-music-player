#include "crank_config.h"
#include "encoder2.h"
#include "potis.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG      = "crank_cfg";
static const char *CFG_PATH = "/sdcard/crank_config.json";

crank_config_t g_crank_cfg;

/* ── factory defaults ──────────────────────────────────────────────────── */

void crank_config_defaults(crank_config_t *c)
{
    c->ema_attack    = 0.025f;
    c->ema_release   = 1.500f;
    c->stop_thresh   = 0.250f;
    c->start_thresh  = 0.700f;
    c->release_ticks = 2;
    c->vol_fade_step  = 1;
    c->lo_bass_weight = 45.0f;
    c->lo_mid_weight  = 5.0f;
    c->lo_decay_rate  = 0.998f;
    c->pot_cal_lo     = 559;
    c->pot_cal_mid    = 945;
    c->pot_cal_hi     = 3071;
}

/* ── load ──────────────────────────────────────────────────────────────── */

void crank_config_load(void)
{
    crank_config_defaults(&g_crank_cfg);

    struct stat st = {};
    if (stat(CFG_PATH, &st) != 0 || st.st_size <= 0) {
        return; /* file absent – silently use defaults */
    }

    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;

    char *buf = (char *)malloc((size_t)st.st_size + 1u);
    if (!buf) { fclose(f); return; }

    size_t n = fread(buf, 1u, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Parse error – using defaults");
        return;
    }

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

    read_f (root, "ema_attack",    0.005f, 0.500f, &g_crank_cfg.ema_attack);
    read_f (root, "ema_release",   0.500f, 2.000f, &g_crank_cfg.ema_release);
    read_f (root, "stop_thresh",   0.050f, 0.600f, &g_crank_cfg.stop_thresh);
    read_f (root, "start_thresh",  0.200f, 1.200f, &g_crank_cfg.start_thresh);
    read_u8(root, "release_ticks", 0, 10, &g_crank_cfg.release_ticks);
    read_u8(root, "vol_fade_step",  1,   10,  &g_crank_cfg.vol_fade_step);
    read_f (root, "lo_bass_weight", 1.0f, 200.0f, &g_crank_cfg.lo_bass_weight);
    read_f (root, "lo_mid_weight",  0.0f,  50.0f, &g_crank_cfg.lo_mid_weight);
    read_f (root, "lo_decay_rate",  0.990f, 0.999f, &g_crank_cfg.lo_decay_rate);
    read_u16(root, "pot_cal_lo",    0, 4095, &g_crank_cfg.pot_cal_lo);
    read_u16(root, "pot_cal_mid",   0, 4095, &g_crank_cfg.pot_cal_mid);
    read_u16(root, "pot_cal_hi",    0, 4095, &g_crank_cfg.pot_cal_hi);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded: attack=%.3f rel=%.1f stop=%.2f start=%.2f rt=%u fs=%u",
             g_crank_cfg.ema_attack, g_crank_cfg.ema_release,
             g_crank_cfg.stop_thresh, g_crank_cfg.start_thresh,
             (unsigned)g_crank_cfg.release_ticks,
             (unsigned)g_crank_cfg.vol_fade_step);
}

/* ── save ──────────────────────────────────────────────────────────────── */

void crank_config_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddNumberToObject(root, "ema_attack",    (double)g_crank_cfg.ema_attack);
    cJSON_AddNumberToObject(root, "ema_release",   (double)g_crank_cfg.ema_release);
    cJSON_AddNumberToObject(root, "stop_thresh",   (double)g_crank_cfg.stop_thresh);
    cJSON_AddNumberToObject(root, "start_thresh",  (double)g_crank_cfg.start_thresh);
    cJSON_AddNumberToObject(root, "release_ticks", (double)g_crank_cfg.release_ticks);
    cJSON_AddNumberToObject(root, "vol_fade_step",  (double)g_crank_cfg.vol_fade_step);
    cJSON_AddNumberToObject(root, "lo_bass_weight",  (double)g_crank_cfg.lo_bass_weight);
    cJSON_AddNumberToObject(root, "lo_mid_weight",   (double)g_crank_cfg.lo_mid_weight);
    cJSON_AddNumberToObject(root, "lo_decay_rate",   (double)g_crank_cfg.lo_decay_rate);
    cJSON_AddNumberToObject(root, "pot_cal_lo",      (double)g_crank_cfg.pot_cal_lo);
    cJSON_AddNumberToObject(root, "pot_cal_mid",     (double)g_crank_cfg.pot_cal_mid);
    cJSON_AddNumberToObject(root, "pot_cal_hi",      (double)g_crank_cfg.pot_cal_hi);

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return;

    FILE *f = fopen(CFG_PATH, "w");
    if (f) {
        fputs(str, f);
        fclose(f);
        ESP_LOGI(TAG, "Config saved");
    } else {
        ESP_LOGE(TAG, "Cannot write %s", CFG_PATH);
    }
    cJSON_free(str);
}

/* ── apply ─────────────────────────────────────────────────────────────── */

void crank_config_apply(void)
{
    encoder2_apply_config(g_crank_cfg.ema_attack, g_crank_cfg.ema_release,
                          g_crank_cfg.stop_thresh, g_crank_cfg.start_thresh,
                          g_crank_cfg.release_ticks);
    potis_set_cal(g_crank_cfg.pot_cal_lo, g_crank_cfg.pot_cal_mid, g_crank_cfg.pot_cal_hi);
}
