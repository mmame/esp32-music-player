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

/**
 * @brief Release I2C bus and free GPIO4 (SCL) for use as BOOT0 output.
 *
 * Safe to call even if dimmerlink_probe() was never called or failed.
 * Pair with dimmerlink_resume() to restore the I2C link.
 */
void dimmerlink_suspend(void);

/**
 * @brief Re-create the I2C bus and device handle after dimmerlink_suspend().
 *
 * Does not re-run the full probe / UART fallback – just reinstates the
 * already-configured bus.  dimmerlink_set_level() will be a no-op if the
 * device is not responding, so this is safe to call unconditionally.
 */
void dimmerlink_resume(void);

#ifdef __cplusplus
}
#endif
