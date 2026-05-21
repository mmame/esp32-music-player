/**
 * @file encoder.cpp
 * @brief Rotary encoder driver using the ESP32-S3 PCNT hardware peripheral.
 *
 * Uses the IDF high-level PCNT driver (driver/pcnt.h) available in ESP-IDF 5.x.
 * Channel A is the primary pulse input; Channel B provides direction via the
 * PCNT "control signal" feature so the hardware counts +1 or -1 per detent
 * without any software glitch filtering.
 *
 * Button press detection uses a GPIO ISR with a simple timestamp debounce.
 * Assumes gpio_install_isr_service() has already been called by the caller
 * (app_main) before encoder_init() is invoked.
 */

#include "encoder.h"
#include "potis.h"

#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "encoder";

/* ── PCNT handle ─────────────────────────────────────────────────────────── */
static pcnt_unit_handle_t    s_pcnt_unit = nullptr;
static pcnt_channel_handle_t s_chan_a    = nullptr;
static pcnt_channel_handle_t s_chan_b    = nullptr;

/* ── Accumulated step counter (updated in ISR / watched task) ──────────── */
static volatile atomic_int s_steps = ATOMIC_VAR_INIT(0);

/* Shadow of the last PCNT hardware count, used to compute delta */
static int s_last_count = 0;

/* Sub-threshold remainder carried between calls so no counts are lost */
static int s_count_remainder = 0;

/* ── Button state ────────────────────────────────────────────────────────── */

static adc_oneshot_unit_handle_t s_btn_adc = nullptr;
static adc_channel_t             s_btn_ch  = ADC_CHANNEL_2;

/* Debounce state */
static int8_t  s_btn_last_id    = -1;  /* raw decoded button from last sample */
static uint8_t s_btn_stable_cnt =  0;  /* # of consecutive identical readings */
static int8_t  s_btn_confirmed  = -1;  /* fully-debounced current button       */
static int8_t  s_btn_reported   = -1;  /* last button for which event was fired */

static const uint16_t s_btn_thresholds[BTN_COUNT] = BTN_THRESHOLDS;

/* Decode a 12-bit ADC raw value to a button index (0–BTN_COUNT-1) or -1. */
static int8_t adc_to_btn(int raw)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        if (raw < (int)s_btn_thresholds[i]) return (int8_t)i;
    }
    return -1; /* above all thresholds – idle */
}

/* ── PCNT overflow callback (keeps s_steps accurate beyond ±32767) ───────── */

static bool pcnt_on_reach(pcnt_unit_handle_t unit,
                           const pcnt_watch_event_data_t *edata,
                           void *user_ctx)
{
    (void)unit; (void)edata; (void)user_ctx;
    return false; /* no higher-priority task woken */
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════════ */

void encoder_init(void)
{
    /* ── PCNT unit ───────────────────────────────────────────────────────── */
    pcnt_unit_config_t unit_cfg = {
        .low_limit  = -32000,
        .high_limit =  32000,
        .flags      = { .accum_count = 1 }, /* Accumulate across overflows */
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_pcnt_unit));

    /* Glitch filter: ignore pulses shorter than 1 µs (1000 ns) */
    pcnt_glitch_filter_config_t filt = {
        .max_glitch_ns = 1000,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt_unit, &filt));

    /* ── Channel A: count edges on A, use B as direction control ────────── */
    pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num  = ENC_PIN_A,
        .level_gpio_num = ENC_PIN_B,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_a_cfg, &s_chan_a));

    /* Rising edge on A: +1 when B=low, -1 when B=high */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_chan_a,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,   /* rising  */
        PCNT_CHANNEL_EDGE_ACTION_DECREASE)); /* falling */
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,      /* B=low  → keep direction */
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE)); /* B=high → invert direction */

    /* ── Channel B: count edges on B, use A as direction control ────────── */
    pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num  = ENC_PIN_B,
        .level_gpio_num = ENC_PIN_A,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_b_cfg, &s_chan_b));

    /* Rising edge on B: -1 when A=low, +1 when A=high (mirror of chan A) */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_chan_b,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_chan_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    /* Register overflow watch points so accumulation is tracked */
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt_unit,  32000));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt_unit, -32000));

    pcnt_event_callbacks_t cbs = {
        .on_reach = pcnt_on_reach,
    };
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(s_pcnt_unit, &cbs, nullptr));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));

    /* ── Button ADC (shares ADC1 handle owned by potis) ──────────────────── */
    s_btn_adc = potis_get_adc_handle();

    adc_unit_t btn_unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BTN_ADC_PIN, &btn_unit, &s_btn_ch));
    if (btn_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "BTN_ADC_PIN (GPIO%d) is not on ADC1 – check pins.h", BTN_ADC_PIN);
    }

    adc_oneshot_chan_cfg_t btn_chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,   /* 0–3.3 V */
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_btn_adc, s_btn_ch, &btn_chan_cfg));

    ESP_LOGI(TAG, "Encoder ready: A=%d B=%d BTN_ADC=GPIO%d (ADC1_CH%d, %d rungs)",
             ENC_PIN_A, ENC_PIN_B, BTN_ADC_PIN, (int)s_btn_ch, BTN_COUNT);
}

int16_t encoder_read_steps(void)
{
    int current = 0;
    pcnt_unit_get_count(s_pcnt_unit, &current);

    /*
     * Accumulate the new delta into the remainder so counts are never lost
     * between polling calls.  Divide by ENC_COUNTS_PER_STEP (default 4 for
     * X4 quadrature; set to 2 or 1 in encoder.h if your encoder is too sluggish).
     */
    int raw_delta     = current - s_last_count;
    int accumulated   = raw_delta + s_count_remainder;
    s_last_count      = current;
    int16_t steps     = (int16_t)(accumulated / ENC_COUNTS_PER_STEP);
    s_count_remainder = accumulated % ENC_COUNTS_PER_STEP;

    if (steps != 0) {
        ESP_LOGD(TAG, "PCNT raw=%d  acc=%d  steps=%d  rem=%d",
                 raw_delta, accumulated, steps, s_count_remainder);
    }
    return steps;
}

int8_t encoder_btn_read(void)
{
    if (!s_btn_adc) return -1;

    int raw = 0;
    adc_oneshot_read(s_btn_adc, s_btn_ch, &raw);
    int8_t id = adc_to_btn(raw);

    /* Debounce: require BTN_DEBOUNCE_SAMPLES consecutive identical readings */
    if (id == s_btn_last_id) {
        if (s_btn_stable_cnt < BTN_DEBOUNCE_SAMPLES) s_btn_stable_cnt++;
    } else {
        s_btn_last_id    = id;
        s_btn_stable_cnt = 1;
    }

    if (s_btn_stable_cnt >= BTN_DEBOUNCE_SAMPLES) {
        s_btn_confirmed = id;
    }

    /* Fire event on new press, arm again on release */
    if (s_btn_confirmed != -1 && s_btn_confirmed != s_btn_reported) {
        s_btn_reported = s_btn_confirmed;
        ESP_LOGD(TAG, "Button %d pressed (raw=%d)", (int)s_btn_confirmed, raw);
        return s_btn_confirmed;
    }
    if (s_btn_confirmed == -1) {
        s_btn_reported = -1; /* re-arm for next press */
    }
    return -1;
}
