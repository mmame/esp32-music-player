/**
 * @file encoder.h
 * @brief Rotary encoder driver using the ESP32 PCNT hardware peripheral.
 *
 * Provides debounce-free reading via hardware pulse counting.
 * Accumulated step counts are read atomically and then cleared.
 *
 * Wire:
 *   ENC_PIN_A  – encoder channel A (any GPIO with input support)
 *   ENC_PIN_B  – encoder channel B
 *   ENC_PIN_BTN – encoder push-button (active-low with internal pull-up)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimum milliseconds between button press events (software debounce) */
#define ENC_BTN_DEBOUNCE_MS  50

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
 * @brief Initialise PCNT unit and button GPIO.
 *        Must be called once before encoder_read_steps() or encoder_btn_pressed().
 */
void encoder_init(void);

/**
 * @brief Return the accumulated step count since the last call and reset it.
 *
 * Positive = clockwise rotation, negative = counter-clockwise.
 * Each detent on a typical 20-PPR encoder produces one count.
 *
 * Thread-safe: uses an atomic compare-and-swap loop internally.
 */
int16_t encoder_read_steps(void);

/**
 * @brief Return true once per button press event (debounced).
 *
 * Calling this function consumes the event; subsequent calls return false
 * until the next press.  Safe to call from any task.
 */
bool encoder_btn_pressed(void);

#ifdef __cplusplus
}
#endif
