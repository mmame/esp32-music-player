/**
 * @file potis.cpp
 * @brief Rheostat reader using the ESP32-S3 ADC oneshot driver (IDF 5.x).
 *
 * Circuit: 3.3 V ── [Rheostat] ──┬── [1.5 kΩ series] ── GND
 *                                 └── ADC pin
 *
 * raw_to_pct() uses a 3-point piecewise-linear calibration (lo/mid/hi raw
 * ADC values measured at the physical min / centre / max knob positions).
 * Defaults match the circuit model; the web calibration wizard overwrites them.
 *
 * Uses the IDF high-level "ADC Oneshot" driver (esp_adc/adc_oneshot.h).
 * Attenuation: ADC_ATTEN_DB_12 → 0–3.3 V input range.
 */

#include "potis.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "potis";

/* ── Internal state ─────────────────────────────────────────────────────── */

static adc_oneshot_unit_handle_t s_adc_handle = nullptr;

static adc_channel_t s_vol_ch = ADC_CHANNEL_0;

static uint16_t s_vol_buf[POT_AVG_SAMPLES];
static uint8_t  s_buf_idx  = 0;
static bool     s_buf_full = false;

static uint8_t s_last_volume = 0xFF;

/* 3-point calibration: raw ADC values at physical min / center / max knob
 * positions.  Defaults match the circuit model at the physical end-stops
 * (R_rheo ≈ R_MAX  → raw ≈ 559; R_rheo = R_MAX/2 → raw ≈ 945;
 *  R_rheo ≈ 0      → raw ≈ 3071).  potis_set_cal() overwrites these. */
static uint16_t s_cal_lo  = 559;
static uint16_t s_cal_mid = 945;
static uint16_t s_cal_hi  = 3071;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Piecewise-linear mapping using 3-point calibration. */
static uint8_t raw_to_pct(uint32_t avg_raw)
{
    float raw = (float)avg_raw;
    float lo  = (float)s_cal_lo;
    float mid = (float)s_cal_mid;
    float hi  = (float)s_cal_hi;
    if (raw <= lo) return 0;
    if (raw >= hi) return 100;
    float pct;
    if (raw <= mid) {
        pct = 50.0f * (raw - lo) / (mid - lo);
    } else {
        pct = 50.0f + 50.0f * (raw - mid) / (hi - mid);
    }
    if (pct <= 0.0f)   return 0;
    if (pct >= 100.0f) return 100;
    return (uint8_t)(pct + 0.5f);
}

static uint32_t buf_raw_avg(void)
{
    uint8_t n = s_buf_full ? POT_AVG_SAMPLES : (s_buf_idx == 0 ? 1u : s_buf_idx);
    uint32_t sum = 0;
    for (uint8_t i = 0; i < n; i++) sum += s_vol_buf[i];
    return (n > 0) ? (sum / n) : 0u;
}

static uint8_t buf_average(void)
{
    return raw_to_pct(buf_raw_avg());
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════════ */

void potis_init(void)
{
    adc_unit_t vol_unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(POT_PIN_VOLUME, &vol_unit, &s_vol_ch));

    if (vol_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "Volume GPIO must belong to ADC1 – check pins.h");
    }

    ESP_LOGI(TAG, "GPIO%d → ADC1_CH%d (volume)", POT_PIN_VOLUME, s_vol_ch);

    /* Initialise ADC1 in oneshot mode */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_vol_ch, &chan_cfg));

    memset(s_vol_buf, 0, sizeof(s_vol_buf));

    ESP_LOGI(TAG, "Potis ready");
}

bool potis_read(uint8_t *out_volume)
{
    int raw_vol = 0;
    adc_oneshot_read(s_adc_handle, s_vol_ch, &raw_vol);

    s_vol_buf[s_buf_idx] = (uint16_t)raw_vol;
    s_buf_idx = (uint8_t)((s_buf_idx + 1) % POT_AVG_SAMPLES);
    if (s_buf_idx == 0) s_buf_full = true;

    uint8_t vol = buf_average();

    if (out_volume) *out_volume = vol;

    int dv = (int)vol - (int)s_last_volume;
    if (dv < 0) dv = -dv;
    bool changed = (dv > POT_CHANGE_THRESHOLD);
    if (changed) s_last_volume = vol;
    return changed;
}

adc_oneshot_unit_handle_t potis_get_adc_handle(void)
{
    return s_adc_handle;
}

void potis_set_cal(uint16_t raw_lo, uint16_t raw_mid, uint16_t raw_hi)
{
    s_cal_lo  = raw_lo;
    s_cal_mid = raw_mid;
    s_cal_hi  = raw_hi;
}

uint16_t potis_read_raw_avg(void)
{
    return (uint16_t)buf_raw_avg();
}
