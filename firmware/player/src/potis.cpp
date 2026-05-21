/**
 * @file potis.cpp
 * @brief Rheostat reader using the ESP32-S3 ADC oneshot driver (IDF 5.x).
 *
 * Circuit per channel:
 *   3.3 V ── [Rheostat 0–POT_R_MAX Ω] ──┬── [POT_R_SERIES] ── GND
 *                                        └── ADC pin
 *
 * The voltage divider produces a non-linear ADC reading for a linear knob
 * position θ.  raw_to_pct() applies the exact inverse transfer function so
 * the caller receives a linear 0–100 value regardless of circuit loading.
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

/* Resolved ADC channel numbers (derived from GPIO numbers at init time) */
static adc_channel_t s_vol_ch = ADC_CHANNEL_0;
static adc_channel_t s_tmp_ch = ADC_CHANNEL_1;

/* Circular moving-average buffers */
static uint16_t s_vol_buf[POT_AVG_SAMPLES];
static uint16_t s_tmp_buf[POT_AVG_SAMPLES];
static uint8_t  s_buf_idx  = 0;
static bool     s_buf_full = false;

/* Last reported values – used to detect changes */
static uint8_t s_last_volume = 0xFF;
static uint8_t s_last_tempo  = 0xFF;

/* ── Rheostat circuit constants ──────────────────────────────────────────── */

/* Current-limiting series resistor between 3.3 V rail and ADC pin [Ω] */
static constexpr float POT_R_SERIES = 1500.0f;
/* Rheostat full-scale resistance [Ω] */
static constexpr float POT_R_MAX    = 10000.0f;
/* 12-bit ADC full scale */
static constexpr float POT_ADC_FS   = 4095.0f;

/* End-stop calibration: theoretical % the formula produces at the physical
 * mechanical stops.  Real rheostats don't reach exactly 0 Ω / R_MAX, so the
 * raw formula may output ~2 % at one end and ~98 % at the other.
 * These constants stretch that practical range back to a true 0–100 scale.
 * If your bar stops short of 0 or 100, adjust the values to match what the
 * bar actually reads when the knob is pressed against each stop. */
static constexpr float POT_STOP_LO  =  5.0f;   /* formula output at min-R stop  */
static constexpr float POT_STOP_HI  = 95.0f;   /* formula output at max-R stop  */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/**
 * Convert a mean 12-bit ADC value to a linear 0–100 knob position.
 *
 * Transfer function of the voltage divider:
 *   raw = ADC_FS × R_rheo / (R_series + R_rheo)
 *
 * Circuit: 3.3V → [R_rheo] → ADC → [R_series] → GND
 * Inverse (knob position θ = R_rheo / R_max):
 *   θ = (R_series / R_max) × (ADC_FS − raw) / raw
 *   pct = θ × 100
 */
static uint8_t raw_to_pct(uint32_t avg_raw)
{
    if (avg_raw == 0) return 0;                        /* open circuit / full-scale R */
    float numer = POT_ADC_FS - (float)avg_raw;
    if (numer <= 0.0f) return 100;                     /* rheostat at 0 Ω  → 100%   */
    float pct    = 100.0f * (POT_R_SERIES / POT_R_MAX) * numer / (float)avg_raw;
    float result = 100.0f - pct;                       /* invert: high R → low %     */
    /* Stretch [POT_STOP_LO … POT_STOP_HI] → [0 … 100] so physical end-stops
     * reach exactly 0 % and 100 % despite real rheostat imperfections. */
    result = (result - POT_STOP_LO) * (100.0f / (POT_STOP_HI - POT_STOP_LO));
    if (result <= 0.0f)   return 0;
    if (result >= 100.0f) return 100;
    return (uint8_t)(result + 0.5f);
}

static uint8_t buf_average(const uint16_t *buf, uint8_t n)
{
    uint32_t sum = 0;
    for (uint8_t i = 0; i < n; i++) sum += buf[i];
    return raw_to_pct(sum / n);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════════ */

void potis_init(void)
{
    /* Resolve GPIO numbers → ADC unit + channel (GPIO ≠ channel on ESP32-S3) */
    adc_unit_t vol_unit, tmp_unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(POT_PIN_VOLUME, &vol_unit, &s_vol_ch));
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(POT_PIN_TEMPO,  &tmp_unit, &s_tmp_ch));

    if (vol_unit != ADC_UNIT_1 || tmp_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "Both poti GPIOs must belong to ADC1 – check pins.h");
    }

    ESP_LOGI(TAG, "GPIO%d → ADC1_CH%d (volume),  GPIO%d → ADC1_CH%d (tempo)",
             POT_PIN_VOLUME, s_vol_ch, POT_PIN_TEMPO, s_tmp_ch);

    /* Initialise ADC1 in oneshot mode */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    /* Configure each channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,   /* 0–3.3 V full-range                  */
        .bitwidth = ADC_BITWIDTH_12,   /* 12-bit resolution (0–4095)           */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_vol_ch, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_tmp_ch, &chan_cfg));

    /* Zero-fill filter buffers */
    memset(s_vol_buf, 0, sizeof(s_vol_buf));
    memset(s_tmp_buf, 0, sizeof(s_tmp_buf));

    ESP_LOGI(TAG, "Potis ready");
}

bool potis_read(uint8_t *out_volume, uint8_t *out_tempo)
{
    int raw_vol = 0, raw_tmp = 0;
    adc_oneshot_read(s_adc_handle, s_vol_ch, &raw_vol);
    adc_oneshot_read(s_adc_handle, s_tmp_ch, &raw_tmp);

    s_vol_buf[s_buf_idx] = (uint16_t)raw_vol;
    s_tmp_buf[s_buf_idx] = (uint16_t)raw_tmp;
    s_buf_idx = (uint8_t)((s_buf_idx + 1) % POT_AVG_SAMPLES);
    if (s_buf_idx == 0) s_buf_full = true;

    uint8_t n = s_buf_full ? POT_AVG_SAMPLES : (s_buf_idx == 0 ? 1 : s_buf_idx);

    uint8_t vol = buf_average(s_vol_buf, n);
    uint8_t tmp = buf_average(s_tmp_buf, n);

    if (out_volume) *out_volume = vol;
    if (out_tempo)  *out_tempo  = tmp;

    /* Report change only if above threshold */
    int dv = (int)vol - (int)s_last_volume;
    int dt = (int)tmp - (int)s_last_tempo;
    if (dv < 0) dv = -dv;
    if (dt < 0) dt = -dt;

    bool changed = (dv > POT_CHANGE_THRESHOLD || dt > POT_CHANGE_THRESHOLD);
    if (changed) {
        s_last_volume = vol;
        s_last_tempo  = tmp;
    }
    return changed;
}

adc_oneshot_unit_handle_t potis_get_adc_handle(void)
{
    return s_adc_handle;
}
