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

/* Moving-average window size (power of 2 recommended for cheap division) */
#define POT_AVG_SAMPLES  32

/* Minimum change (0–100 scale) before a new value is considered "changed" */
#define POT_CHANGE_THRESHOLD  2

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise ADC channels and internal filter buffers.
 *        Must be called once before potis_read().
 */
void potis_init(void);

/**
 * @brief Sample the volume potentiometer and update the moving-average filter.
 *        Call this periodically (e.g. every 10 ms from Core 0 task).
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

#ifdef __cplusplus
}
#endif
