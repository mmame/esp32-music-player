/**
 * @file potis.h
 * @brief Potentiometer reader with moving-average filter.
 *
 * Reads two analog potentiometers:
 *   Pot 1 (POT_PIN_VOLUME)  → Master volume  (0–100)
 *   Pot 2 (POT_PIN_TEMPO)   → Playback speed (0–100, maps 0.5×–2.0× externally)
 *
 * Filtering: simple N-sample moving average reduces ADC noise.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Moving-average window size (power of 2 recommended for cheap division) */
#define POT_AVG_SAMPLES  8

/* Minimum change (0–100 scale) before a new value is considered "changed" */
#define POT_CHANGE_THRESHOLD  2

/* ── API ──────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialise ADC channels and internal filter buffers.
 *        Must be called once before potis_read().
 */
void potis_init(void);

/**
 * @brief Sample both potentiometers and update the moving-average filters.
 *        Call this periodically (e.g. every 10 ms from Core 0 task).
 *
 * @param[out] volume  Filtered volume value 0–100 (or NULL to ignore).
 * @param[out] tempo   Filtered tempo  value 0–100 (or NULL to ignore).
 * @return true if either value changed by more than POT_CHANGE_THRESHOLD
 *         since the last call where a change was reported.
 */
bool potis_read(uint8_t *volume, uint8_t *tempo);

#ifdef __cplusplus
}
#endif
