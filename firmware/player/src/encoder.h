/**
 * @file encoder.h
 * @brief Rotary encoder driver using the ESP32 PCNT hardware peripheral.
 *
 * Provides debounce-free rotation reading via hardware pulse counting.
 * Button detection uses ADC polling on a resistor-ladder (see BTN_ADC_PIN /
 * BTN_THRESHOLDS in pins.h) so up to BTN_COUNT independent buttons are
 * supported on a single GPIO.
 *
 * Wire:
 *   ENC_PIN_A   – encoder channel A (any GPIO with input support)
 *   ENC_PIN_B   – encoder channel B
 *   BTN_ADC_PIN – resistor-ladder ADC input (no internal pull-up)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Consecutive identical ADC readings required before a button is confirmed */
#define BTN_DEBOUNCE_SAMPLES  3

/*
 * Raw PCNT edges per reported step.
 * X4 quadrature (both edges, both channels) produces 4 counts per detent
 * on a standard EC11 encoder.  If your encoder reports steps only after
 * spinning a lot, halve this value (try 2, then 1).
 * If it reports multiple steps per detent, double it.
 */
#define ENC_COUNTS_PER_STEP  4

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise PCNT unit and ADC button channel.
 *        potis_init() MUST be called first (shares the ADC1 handle).
 *        Assumes gpio_install_isr_service() has already been called.
 */
void encoder_init(void);

/**
 * @brief Return the accumulated rotation step count since the last call.
 *
 * Positive = clockwise, negative = counter-clockwise.
 * Thread-safe (atomic compare-and-swap loop).
 */
int16_t encoder_read_steps(void);

/**
 * @brief Sample the ADC-ladder buttons (call from io_task every ~10 ms).
 *
 * Returns the index of a newly confirmed button press (0 = closest to GND,
 * BTN_COUNT-1 = highest voltage rung), or -1 if no new event.
 *
 * One event is generated per press; holding the button does NOT repeat.
 * The event is re-armed after the button is released.
 */
int8_t encoder_btn_read(void);

#ifdef __cplusplus
}
#endif
