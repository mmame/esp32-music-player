/**
 * @file soundtouch_el.h
 * @brief ADF audio element wrapper around the SoundTouch library.
 *
 * Replaces the ADF sonic element with SoundTouch's TDHS time-stretching
 * algorithm, which gives significantly better audio quality.
 *
 * Usage:
 *   soundtouch_el_cfg_t cfg = SOUNDTOUCH_EL_DEFAULT_CFG();
 *   cfg.samplerate = 44100;
 *   cfg.channels   = 2;
 *   cfg.tempo      = 1.0f;
 *   audio_element_handle_t el = soundtouch_el_init(&cfg);
 *   // register in pipeline like any other element
 *   // change tempo at runtime:
 *   soundtouch_el_set_tempo(el, 1.25f);
 */
#pragma once

#include "audio_element.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   samplerate;    /*!< Sample rate in Hz (e.g. 44100)                  */
    int   channels;      /*!< 1 = mono, 2 = stereo                            */
    float tempo;         /*!< Initial tempo: 1.0 = normal, 2.0 = 2× speed    */
    int   out_rb_size;   /*!< Output ring-buffer size in bytes                */
    int   task_stack;    /*!< Element task stack in bytes                     */
    int   task_core;     /*!< CPU core for element task (0 or 1)              */
    int   task_prio;     /*!< Element task priority                           */
    bool  stack_in_ext;  /*!< Allocate task stack in external (PSRAM) memory  */
} soundtouch_el_cfg_t;

#define SOUNDTOUCH_EL_DEFAULT_CFG() {  \
    .samplerate   = 44100,             \
    .channels     = 2,                 \
    .tempo        = 1.0f,              \
    .out_rb_size  = 16 * 1024,         \
    .task_stack   = 16 * 1024,         \
    .task_core    = 0,                 \
    .task_prio    = 5,                 \
    .stack_in_ext = true,              \
}

/**
 * @brief  Create and initialise a SoundTouch audio element.
 * @param  cfg  Configuration (must not be NULL).
 * @return audio element handle, or NULL on failure.
 */
audio_element_handle_t soundtouch_el_init(const soundtouch_el_cfg_t *cfg);

/**
 * @brief  Change the playback tempo at runtime.
 *
 * Thread-safe: may be called from any task.  The new tempo is applied
 * at the start of the next processing chunk (typically < 12 ms latency).
 *
 * @param  self   Element handle returned by soundtouch_el_init().
 * @param  tempo  New tempo factor (1.0 = normal, 2.0 = double speed, …).
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t soundtouch_el_set_tempo(audio_element_handle_t self, float tempo);

/**
 * @brief  Enable or disable the SoundTouch bypass (passthrough) mode.
 *
 * When bypass is true the element copies PCM samples from its input ring
 * buffer directly to its output ring buffer without involving SoundTouch at
 * all.  This guarantees bit-perfect, zero-artifact audio regardless of the
 * current tempo setting.
 *
 * When bypass transitions from true back to false, SoundTouch's internal
 * state is cleared so no stale data leaks into the resumed output.
 *
 * Thread-safe: may be called from any task.  Takes effect at the start of
 * the next processing chunk (typically < 12 ms latency).
 *
 * @param  self    Element handle returned by soundtouch_el_init().
 * @param  bypass  true = passthrough (no processing), false = normal mode.
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t soundtouch_el_set_bypass(audio_element_handle_t self, bool bypass);

#ifdef __cplusplus
}
#endif
