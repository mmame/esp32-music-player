/**
 * @file dimmerlink.h
 * @brief Startup probe for the RBDimmer DimmerLink I2C module.
 *
 * Initialises I2C master bus 0 on the pins defined in pins.h
 * (DIMMERLINK_SDA_PIN / DIMMERLINK_SCL_PIN), checks for the device at
 * the default 7-bit address 0x50, then reads and logs the device status.
 *
 * Call dimmerlink_probe() once during app_main.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Probe for the DimmerLink I2C module and log its status.
 *
 * @return true   Device found; status has been logged via ESP_LOG.
 * @return false  Device not present or I2C bus initialisation failed.
 */
bool dimmerlink_probe(void);

/**
 * @brief Set the DimmerLink brightness.
 *
 * Safe to call even if dimmerlink_probe() returned false (no-op in that case).
 *
 * @param pct  Brightness 0–100 (percent).  Values > 100 are clamped.
 */
void dimmerlink_set_level(uint8_t pct);

#ifdef __cplusplus
}
#endif
