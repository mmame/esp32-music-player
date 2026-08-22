/**
 * @file crank_config.h
 * @brief Runtime-configurable crank smoothing parameters.
 *
 * Parameters are persisted as /sdcard/crank_config.json between reboots.
 * Call crank_config_load() once from app_main after the SD card is mounted,
 * then crank_config_apply() after encoder2_init().
 * The web POST handler updates g_crank_cfg and calls both
 * crank_config_save() and crank_config_apply() to apply changes live.
 */
#pragma once
#include <stdint.h>
#include "potis.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float   ema_attack;      /**< EMA α during playback   [0.005–0.500, def 0.025] */
    float   ema_release;     /**< EMA α when crank stops  [0.500–2.000, def 1.500] */
    float   stop_thresh;     /**< pause threshold   [RPS]  [0.050–0.600, def 0.250] */
    float   start_thresh;    /**< resume threshold  [RPS]  [0.200–1.200, def 0.700] */
    uint8_t release_ticks;   /**< zero-windows before fast decay onset [0–10, def 2] */
    uint8_t vol_fade_step;   /**< volume units per 10 ms fade tick [1–10, def 1]    */
    /* Light-organ (FFT) global parameters – only used when per-song light_organ is set */
    float   lo_bass_weight;  /**< sqrtf(bass) multiplier [1–200, def 45]  */
    float   lo_mid_weight;   /**< sqrtf(mid)  multiplier [0– 50, def  5]  */
    float   lo_decay_rate;   /**< auto-range peak decay per 50 ms frame [0.990–0.999, def 0.998] */
    uint16_t pot_cal_lo;     /**< raw ADC at pot minimum stop  [0–4095, def 559]  */
    uint16_t pot_cal_mid;    /**< raw ADC at pot center knob   [0–4095, def 945]  */
    uint16_t pot_cal_hi;     /**< raw ADC at pot maximum stop  [0–4095, def 3071] */
} crank_config_t;

/** Globally shared config; written by crank_config_load() and the web POST handler. */
extern crank_config_t g_crank_cfg;

/** Fill *c with factory defaults (matching the compile-time constants in encoder2.cpp). */
void crank_config_defaults(crank_config_t *c);

/**
 * Load /sdcard/crank_config.json into g_crank_cfg.
 * Falls back to defaults if the file is absent or malformed.
 */
void crank_config_load(void);

/** Serialise g_crank_cfg to /sdcard/crank_config.json. */
void crank_config_save(void);

/**
 * Push g_crank_cfg into the encoder2 driver.
 * Call after crank_config_load() and after every web-triggered update.
 */
void crank_config_apply(void);

#ifdef __cplusplus
}
#endif
