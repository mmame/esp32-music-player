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
 */

#include "encoder.h"
#include "compat.h"

#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "encoder";

/* ── PCNT handle ─────────────────────────────────────────────────────────── */
static pcnt_unit_handle_t  s_pcnt_unit = nullptr;
static pcnt_channel_handle_t s_chan_a  = nullptr;
static pcnt_channel_handle_t s_chan_b  = nullptr;

/* ── Accumulated step counter (updated in ISR / watched task) ──────────── */
static volatile atomic_int s_steps = ATOMIC_VAR_INIT(0);

/* Shadow of the last PCNT hardware count, used to compute delta */
static int s_last_count = 0;

/* Sub-threshold remainder carried between calls so no counts are lost */
static int s_count_remainder = 0;

/* ── Button state ────────────────────────────────────────────────────────── */
static volatile bool     s_btn_event    = false;
static volatile uint32_t s_btn_last_ms  = 0;

/* ── Button GPIO ISR ─────────────────────────────────────────────────────── */

static void IRAM_ATTR btn_isr_handler(void *arg)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((now - s_btn_last_ms) >= ENC_BTN_DEBOUNCE_MS) {
        s_btn_last_ms = now;
        s_btn_event   = true;
    }
}

/* ── PCNT overflow callback (keeps s_steps accurate beyond ±32767) ───────── */

static bool pcnt_on_reach(pcnt_unit_handle_t unit,
                           const pcnt_watch_event_data_t *edata,
                           void *user_ctx)
{
    (void)unit; (void)edata; (void)user_ctx;
    /* Nothing: we use pcnt_unit_get_count() polling in encoder_read_steps() */
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

    /* ── Button GPIO ─────────────────────────────────────────────────────── */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << ENC_PIN_BTN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,   /* Trigger on falling edge (press) */
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)ENC_PIN_BTN, btn_isr_handler, nullptr));

    ESP_LOGI(TAG, "Encoder ready: A=%d B=%d BTN=%d", ENC_PIN_A, ENC_PIN_B, ENC_PIN_BTN);
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
    int raw_delta      = current - s_last_count;
    int accumulated    = raw_delta + s_count_remainder;
    s_last_count       = current;
    int16_t steps      = (int16_t)(accumulated / ENC_COUNTS_PER_STEP);
    s_count_remainder  = accumulated % ENC_COUNTS_PER_STEP;

    if (steps != 0) {
        ESP_LOGD(TAG, "PCNT raw=%d  acc=%d  steps=%d  rem=%d",
                 raw_delta, accumulated, steps, s_count_remainder);
    }
    return steps;
}

bool encoder_btn_pressed(void)
{
    if (s_btn_event) {
        s_btn_event = false;
        return true;
    }
    return false;
}
