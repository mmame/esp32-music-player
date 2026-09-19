/**
 * @file limiter_el.h
 * @brief ADF audio element: static output gain (0-10 dB) with a peak limiter.
 *
 * Wraps esp_ae_drc.  The DRC curve is a hard-knee limiter whose ceiling sits
 * 1 dB below full scale *after* the make-up gain, so boosting by gain_db can
 * never clip the I2S output:
 *
 *   curve : (-100,-100) (T,T) (0,T)        with T = -(gain_db + 1)
 *   makeup: +gain_db                       -> output ceiling = -1 dBFS
 *
 * Negative gains are a plain attenuation (identity curve, negative make-up gain).
 *
 * Place it as the LAST element before i2s_stream (behind the volume stage).
 * gain_db == 0 is a bit-perfect passthrough (DRC not run).
 */
#pragma once

#include "audio_element.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIMITER_EL_MAX_GAIN_DB   10   /* esp_ae_drc make-up gain limit */
#define LIMITER_EL_MIN_GAIN_DB  (-10)

typedef struct {
    int   samplerate;    /*!< Sample rate in Hz (only shapes attack/release)  */
    int   channels;      /*!< 1 = mono, 2 = stereo (interleaved 16-bit)       */
    int   out_rb_size;   /*!< Output ring-buffer size in bytes                */
    int   task_stack;    /*!< Element task stack in bytes                     */
    int   task_core;     /*!< CPU core for element task (0 or 1)              */
    int   task_prio;     /*!< Element task priority                           */
    bool  stack_in_ext;  /*!< Allocate task stack in external (PSRAM) memory  */
} limiter_el_cfg_t;

#define LIMITER_EL_DEFAULT_CFG() {  \
    .samplerate   = 48000,          \
    .channels     = 1,              \
    .out_rb_size  = 16 * 1024,      \
    .task_stack   = 4 * 1024,       \
    .task_core    = 0,              \
    .task_prio    = 5,              \
    .stack_in_ext = true,           \
}

/** Create the element.  Returns NULL on failure. */
audio_element_handle_t limiter_el_init(const limiter_el_cfg_t *cfg);

/**
 * @brief  Set the static gain in dB (0 = bypass; clamped to
 *         LIMITER_EL_MIN_GAIN_DB..LIMITER_EL_MAX_GAIN_DB).
 *
 * Thread-safe; takes effect at the start of the next processed chunk.
 */
esp_err_t limiter_el_set_gain_db(audio_element_handle_t self, int8_t gain_db);

#ifdef __cplusplus
}
#endif
