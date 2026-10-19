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
#include "esp_timer.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "potis";

/* ── Internal state ─────────────────────────────────────────────────────── */

static adc_oneshot_unit_handle_t s_adc_handle = nullptr;

static adc_channel_t s_vol_ch = ADC_CHANNEL_0;

static volatile uint16_t s_vol_buf[POT_AVG_SAMPLES];   /* written by the sampling timer, read by potis_read() */
static volatile uint32_t s_buf_cnt = 0;                /* total samples taken (index = cnt % N) */

static uint8_t s_last_volume = 0xFF;

/* 3-point calibration: raw ADC values at physical min / center / max knob
 * positions.  Defaults match the circuit model at the physical end-stops
 * (R_rheo ≈ R_MAX  → raw ≈ 559; R_rheo = R_MAX/2 → raw ≈ 945;
 *  R_rheo ≈ 0      → raw ≈ 3071).  potis_set_cal() overwrites these. */
static uint16_t s_cal_lo  = 559;
static uint16_t s_cal_mid = 945;
static uint16_t s_cal_hi  = 3071;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Knob position 0..1 from the raw ADC value.
 *
 * The rheostat sits in a voltage divider (see the circuit above), so raw = hi / (1 + k * (1 - a)) with
 * k = hi / lo - 1 and a = knob position: hyperbolic, not linear.  A piecewise-linear map of raw would make
 * the lower half of the travel far too quiet.  Inverting the model gives a ~linear knob position; the
 * calibrated centre value then warps it so that "mid" is exactly 50 %, which also absorbs a pot taper that
 * differs from the model. */
static float raw_to_pos(float raw, float lo, float hi)
{
    const float k = hi / lo - 1.0f;
    float a = 1.0f - (hi / raw - 1.0f) / k;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
}

static uint8_t raw_to_pct(uint32_t avg_raw)
{
    float raw = (float)avg_raw;
    float lo  = (float)s_cal_lo;
    float mid = (float)s_cal_mid;
    float hi  = (float)s_cal_hi;
    if (raw <= lo) return 0;
    if (raw >= hi) return 100;
    float pct;
    if (lo > 0.0f && hi > lo * 1.05f && mid > lo && mid < hi) {
        const float a     = raw_to_pos(raw, lo, hi);
        const float a_mid = raw_to_pos(mid, lo, hi);
        float pos;
        if (a_mid < 0.05f || a_mid > 0.95f) {
            pos = a;                                                   /* implausible centre: model only */
        } else if (a <= a_mid) {
            pos = 0.5f * a / a_mid;
        } else {
            pos = 0.5f + 0.5f * (a - a_mid) / (1.0f - a_mid);
        }
        pct = 100.0f * pos;
    } else if (raw <= mid) {                                           /* degenerate calibration: old mapping */
        pct = 50.0f * (raw - lo) / (mid - lo);
    } else {
        pct = 50.0f + 50.0f * (raw - mid) / (hi - mid);
    }
    /* the divider is hyperbolic: at the bottom of the travel the raw value moves only a few counts per percent, so
     * the "almost zero" margin has to be applied on the linearized position, not on the raw value */
    const float dead = 3.5f;                       /* dead window at the bottom: reads 0 (mute) */
    if (pct <= dead)   return 0;
    if (pct >= 100.0f) return 100;
    /* compress the live range so it starts at 1 % right after the dead window (no jump from 0 to 3-4 %) */
    pct = 1.0f + (pct - dead) * (99.0f / (100.0f - dead));
    return (uint8_t)(pct + 0.5f);
}

static uint32_t buf_raw_avg(void)
{
    const uint32_t cnt = s_buf_cnt;
    const uint32_t n   = cnt < POT_AVG_SAMPLES ? cnt : POT_AVG_SAMPLES;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += s_vol_buf[i];
    return (n > 0) ? (sum / n) : 0u;
}

/* esp_timer callback (esp_timer task): one ADC sample into the ring */
static void sample_cb(void *)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, s_vol_ch, &raw) != ESP_OK) return;
    const uint32_t c = s_buf_cnt;
    s_vol_buf[c % POT_AVG_SAMPLES] = (uint16_t)raw;
    s_buf_cnt = c + 1;
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

    for (int i = 0; i < 16; i++) sample_cb(nullptr);    /* valid average from the first potis_read() */

    const esp_timer_create_args_t targs = { .callback = sample_cb, .arg = nullptr,
                                            .dispatch_method = ESP_TIMER_TASK, .name = "pot_adc" };
    esp_timer_handle_t th = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &th));
    ESP_ERROR_CHECK(esp_timer_start_periodic(th, POT_SAMPLE_US));

    ESP_LOGI(TAG, "Potis ready");
}

bool potis_read(uint8_t *out_volume)
{
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
