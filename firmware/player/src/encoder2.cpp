/**
 * @file encoder2.cpp
 * @brief Organ-encoder driver: PCNT speed measurement with EMA smoothing.
 *
 * Reads the 2nd rotary encoder wired to ENC2_PIN_A / ENC2_PIN_B.
 * The encoder has 360 quadrature cycles per revolution; in X4 counting mode
 * that is 1440 PCNT edges/revolution.
 *
 * encoder2_update() is called every ~10 ms from io_task.  It computes an
 * instant speed from the PCNT delta and applies an exponential moving average
 * (EMA) with a ~45 ms time constant, giving smooth speed transitions without
 * audible clicks.
 *
 * Hysteretic thresholds:
 *   Stopped → Moving :  smoothed speed > 0.35 RPS
 *   Moving  → Stopped:  smoothed speed < 0.25 RPS
 *
 * Speed mapping (done by the caller, io_task):
 *   Returned RPS is used directly as the playback speed multiplier
 *   (1 RPS ≈ 1.0× normal speed), clamped to [SPEED_MIN, SPEED_MAX].
 */

#include "encoder2.h"
#include "pins.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "enc2";

/* ── Tuning constants ────────────────────────────────────────────────────── */

/* 360 quad cycles/rev × 4 PCNT edges = 1440 counts per full revolution */
#define ENC2_COUNTS_PER_REV  1440.0f

/* io_task tick period [s] – must match vTaskDelay(pdMS_TO_TICKS(10)) */
#define ENC2_TICK_DT         0.010f

/*
 * Number of io_task ticks accumulated into one speed sample.
 * 5 × 10 ms = 50 ms measurement window per EMA update.
 */
#define ENC2_WINDOW_TICKS    5
#define ENC2_DT              (ENC2_TICK_DT * ENC2_WINDOW_TICKS)  /* 0.050 s */

/*
 * EMA smoothing factor α (applied once per 50 ms window).
 * Three-phase asymmetric envelope:
 *   STARTUP  – fast climb from 0 to threshold.    τ ≈  0.6 s  (not yet moving)
 *   ATTACK   – slow speed changes during playback. τ ≈ 12.5 s  (already moving)
 *   RELEASE  – fast decay when crank stops.        τ ≈  172 ms
 */
#define ENC2_EMA_ALPHA_STARTUP   1.500f
#define ENC2_EMA_ALPHA_ATTACK    0.025f
#define ENC2_EMA_ALPHA_RELEASE   1.500f

/*
 * Consecutive zero-count windows required before switching to fast release.
 * 2 windows × 50 ms = 100 ms of silence before decay kicks in.
 */
#define ENC2_RELEASE_TICKS  2

/* Hysteretic stop/start thresholds [RPS] */
#define ENC2_STOP_THRESH     0.25f   /* moving  → stopped (lower threshold) */
#define ENC2_START_THRESH    0.70   /* stopped → moving  (upper threshold) */

/* ── Module state ────────────────────────────────────────────────────────── */

static pcnt_unit_handle_t    s_unit   = nullptr;
static pcnt_channel_handle_t s_chan_a = nullptr;
static pcnt_channel_handle_t s_chan_b = nullptr;

static float   s_speed_smooth = 0.0f;
static float   s_instant_rps  = 0.0f; /* raw speed from last 50 ms window, no EMA */
static bool    s_moving       = false;
static uint8_t s_zero_ticks   = 0;   /* consecutive windows with delta == 0 */
static uint8_t s_tick_count   = 0;   /* ticks accumulated in current window  */
static int     s_delta_acc    = 0;   /* running count sum for current window  */

/* ══════════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════════ */

void encoder2_init(void)
{
    pcnt_unit_config_t unit_cfg = {
        .low_limit  = -32000,
        .high_limit =  32000,
        .flags      = { .accum_count = 1 },
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_unit));

    pcnt_glitch_filter_config_t filt = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &filt));

    gpio_reset_pin((gpio_num_t)ENC2_PIN_A);
    gpio_reset_pin((gpio_num_t)ENC2_PIN_B);

    /* Channel A: count A edges; use B as direction control */
    pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num  = ENC2_PIN_A,
        .level_gpio_num = ENC2_PIN_B,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_a_cfg, &s_chan_a));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_chan_a,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,   /* rising  A */
        PCNT_CHANNEL_EDGE_ACTION_DECREASE)); /* falling A */
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,      /* B = low  → keep direction  */
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE)); /* B = high → invert direction */

    /* Channel B: count B edges; use A as direction control */
    pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num  = ENC2_PIN_B,
        .level_gpio_num = ENC2_PIN_A,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &chan_b_cfg, &s_chan_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(s_chan_b,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(s_chan_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));

    ESP_LOGI(TAG, "Organ encoder ready: A=GPIO%d B=GPIO%d  (%.0f cnts/rev)",
             ENC2_PIN_A, ENC2_PIN_B, ENC2_COUNTS_PER_REV);
}

float encoder2_update(void)
{
    /* Read this tick's count and add to the running window sum. */
    int tick_delta = 0;
    pcnt_unit_get_count(s_unit, &tick_delta);
    pcnt_unit_clear_count(s_unit);
    s_delta_acc += tick_delta;

    /* Only compute speed once per full 50 ms window; return last value early. */
    if (++s_tick_count < ENC2_WINDOW_TICKS) {
        return s_moving ? fabsf(s_speed_smooth) : 0.0f;
    }
    s_tick_count = 0;
    int delta   = s_delta_acc;
    s_delta_acc = 0;

    /* Signed instant speed so direction reversals cancel in the filter. */
    float instant_rps_signed = (float)delta / (ENC2_COUNTS_PER_REV * ENC2_DT);
    /* Store raw (unsmoothed) magnitude for consumers that want instant response
     * (e.g. DimmerLink – deliberately skips the heavy audio EMA). */
    s_instant_rps = fabsf(instant_rps_signed);

    /* Asymmetric EMA: slow attack (organ feel), fast release (crank stopped).
     * Release only engages after ENC2_RELEASE_TICKS consecutive zero-count
     * windows (5 × 50 ms = 250 ms), so brief pauses mid-crank are ignored. */
    if (delta == 0) {
        if (s_zero_ticks < ENC2_RELEASE_TICKS) s_zero_ticks++;
    } else {
        s_zero_ticks = 0;
    }
    /* Three-phase alpha selection:
     *   release  – zero-count silence threshold reached
     *   startup  – crank spinning but playback not yet active (fast climb)
     *   attack   – playback running, smooth out speed changes (slow) */
    float alpha;
    if (s_zero_ticks >= ENC2_RELEASE_TICKS) {
        alpha = ENC2_EMA_ALPHA_RELEASE;
    } else if (!s_moving) {
        alpha = ENC2_EMA_ALPHA_STARTUP;
    } else {
        alpha = ENC2_EMA_ALPHA_ATTACK;
    }
    s_speed_smooth = alpha * instant_rps_signed
                   + (1.0f - alpha) * s_speed_smooth;

    float speed_abs = fabsf(s_speed_smooth);

    /* Hysteretic stopped / moving state machine */
    if (!s_moving && speed_abs > ENC2_START_THRESH) {
        s_moving = true;
    } else if (s_moving && speed_abs < ENC2_STOP_THRESH) {
        s_moving = false;
    }

    return s_moving ? speed_abs : 0.0f;
}

bool encoder2_is_moving(void)
{
    return s_moving;
}

float encoder2_get_instant_rps(void)
{
    return s_instant_rps;
}
