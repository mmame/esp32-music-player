/**
 * @file potis.h
 * @brief Potentiometer reader with moving-average filter.
 *
 * Reads one analog potentiometer:
 *   Pot 1 (POT_PIN_VOLUME)  → Master volume  (0–100)
 *
 * Filtering: simple N-sample moving average reduces ADC noise.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "pins.h"

#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Moving-average window: samples are taken in the background every POT_SAMPLE_US, so the window spans
 * POT_AVG_SAMPLES * POT_SAMPLE_US = 128 * 2 ms = ~256 ms (4x more samples than the old 32 x 10 ms). */
#define POT_AVG_SAMPLES  128
#define POT_SAMPLE_US    2000

/* Minimum change (0–100 scale) before a new value is considered "changed" */
#define POT_CHANGE_THRESHOLD  0

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise ADC channels and internal filter buffers.
 *        Must be called once before potis_read().
 */
void potis_init(void);

/**
 * @brief Read the filtered volume potentiometer.  The ADC is sampled by a background timer
 *        (see POT_SAMPLE_US); this only averages the collected samples, so call it at any rate.
 *
 * @param[out] volume  Filtered volume value 0–100 (or NULL to ignore).
 * @return true if the value changed by more than POT_CHANGE_THRESHOLD
 *         since the last call where a change was reported.
 */
bool potis_read(uint8_t *volume);

/**
 * @brief Return the ADC1 oneshot handle.
 *        Other modules (e.g. encoder button ADC) may add channels to the
 *        same ADC unit by calling adc_oneshot_config_channel() on this handle.
 *        Call only after potis_init().
 */
adc_oneshot_unit_handle_t potis_get_adc_handle(void);

/**
 * @brief Update the 3-point calibration used by raw_to_pct().
 *        raw_lo < raw_mid < raw_hi must hold; mapping: lo→0%, mid→50%, hi→100%.
 *        Call after potis_init(); safe to call from any task.
 */
void     potis_set_cal(uint16_t raw_lo, uint16_t raw_mid, uint16_t raw_hi);

/**
 * @brief Return the current moving-average raw ADC value (12-bit, 0–4095).
 *        Used by the calibration wizard to capture a stable sample.
 */
uint16_t potis_read_raw_avg(void);

#ifdef __cplusplus
}
#endif
