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

/**
 * @brief  Set the pitch-influence factor at runtime (0.0–1.0).
 *
 * 0.0 = pure time-stretch (pitch unchanged regardless of speed).
 * 1.0 = pure tape effect  (pitch tracks speed 1:1).
 * At factor α and speed S: rate = S^α, tempo = S^(1-α).
 *
 * On any change SoundTouch's internal state is cleared to prevent artefacts.
 * Thread-safe; takes effect at the start of the next processing chunk.
 *
 * @param  self             Element handle returned by soundtouch_el_init().
 * @param  pitch_influence  Blend factor in [0.0, 1.0] (clamped).
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t soundtouch_el_set_pitch_influence(audio_element_handle_t self, float pitch_influence);

/**
 * @brief Set stereo downmix mode used before SoundTouch processing.
 *
 * 0 = mix L+R, 1 = left only, 2 = right only.
 */
esp_err_t soundtouch_el_set_downmix_mode(audio_element_handle_t self, uint8_t mode);

/**
 * @brief Set downmix transition fade duration in milliseconds.
 */
/**
 * @brief Schedule two downmix-mode changes relative to the input consumed from now on (used for crossfades).
 *        At @p after0_frames input frames the mode becomes @p mode0 (immediately), at @p after1_frames it becomes
 *        @p mode1 (with the configured downmix fade if @p arm1).  Cleared when the element is opened.
 */
esp_err_t soundtouch_el_schedule_downmix(audio_element_handle_t self,
                                         uint8_t mode0, uint32_t after0_frames,
                                         uint8_t mode1, uint32_t after1_frames, bool arm1);

esp_err_t soundtouch_el_set_downmix_fade_ms(audio_element_handle_t self, uint16_t fade_ms);

/**
 * @brief Arm/disarm fade for the next mode change.
 *
 * When armed=true and mode changes, transition is faded. Otherwise it is instant.
 */
esp_err_t soundtouch_el_arm_downmix_fade(audio_element_handle_t self, bool armed);

/* ---------------------------------------------------------------------------
 * Benchmark helper (diagnostics): an independent SoundTouch instance configured like the element's
 * (mono, same quality settings), used to measure what a SECOND concurrent stream (crossfade) costs.
 * It has to live in this component because SoundTouch is compiled here with SOUNDTOUCH_INTEGER_SAMPLES.
 * ------------------------------------------------------------------------- */
typedef struct soundtouch_sim *soundtouch_sim_handle_t;

/** Create a mono instance (NULL on out-of-memory). */
soundtouch_sim_handle_t soundtouch_sim_create(int samplerate, float tempo);

/** Feed up to 16384 mono int16 frames and discard whatever comes out; returns the number of frames produced. */
int soundtouch_sim_process(soundtouch_sim_handle_t h, const int16_t *mono, int frames);

void soundtouch_sim_destroy(soundtouch_sim_handle_t h);

#ifdef __cplusplus
}
#endif
