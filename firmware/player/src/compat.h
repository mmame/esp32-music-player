#pragma once
/**
 * ESP-IDF 5.x compatibility shims for arduino-audio-tools.
 *
 * Include this file BEFORE any AudioTools headers so the macros are defined
 * when I2SESP32V1.h is compiled as part of the same translation unit.
 *
 * Constants removed / renamed between ESP-IDF 4.x and 5.1+:
 *   I2S_MCLK_MULTIPLE_192          → I2S_MCLK_MULTIPLE_384  (384 is valid)
 *   I2S_CLK_SRC_EXTERNAL           → I2S_CLK_SRC_XTAL
 *   I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG → I2S_PDM_TX_SLOT_DEFAULT_CONFIG
 */
#include "esp_idf_version.h"

#if ESP_IDF_VERSION_MAJOR >= 5
#  define I2S_MCLK_MULTIPLE_192              I2S_MCLK_MULTIPLE_384
#  define I2S_CLK_SRC_EXTERNAL               I2S_CLK_SRC_XTAL
#  define I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG I2S_PDM_TX_SLOT_DEFAULT_CONFIG
#endif
