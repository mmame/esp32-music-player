/**
 * @file encoder2.h
 * @brief Organ-encoder driver: speed measurement with EMA smoothing.
 *
 * Reads the 2nd rotary encoder (ENC2_PIN_A / ENC2_PIN_B, 360 quad-cycles/rev).
 * Call encoder2_update() every ~10 ms from io_task; it returns a smoothed
 * speed in rotations/second (RPS).  io_task maps RPS → SoundTouch playback
 * speed and drives pause/resume transitions.
 *
 * Reference:  1 RPS  →  1.0× playback speed
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the PCNT unit for encoder 2.
 *        Call once in app_main before io_task starts.
 */
void  encoder2_init(void);

/**
 * @brief Update the internal EMA from the latest PCNT delta.
 *        Must be called every ~10 ms (io_task tick).
 *
 * @return Smoothed speed [rotations/second].
 *         Returns 0.0 when the encoder is considered stopped
 *         (see ENC2_STOP_THRESH / ENC2_START_THRESH in encoder2.cpp).
 */
float encoder2_update(void);

/**
 * @return true while the encoder is spinning fast enough for playback.
 */
bool  encoder2_is_moving(void);

/**
 * @brief Raw (unsmoothed) speed from the last 50 ms measurement window.
 *
 * Unlike encoder2_update(), this value has no EMA applied, so it responds
 * instantly to crank irregularities.  Suitable for driving a lamp dimmer
 * where visible flicker is desirable.  Updated every 50 ms.
 *
 * @return Instant speed [rotations/second], always >= 0.
 */
float encoder2_get_instant_rps(void);

/**
 * @brief Apply runtime-configurable tuning parameters.
 *
 * Replaces the compile-time #define defaults with caller-supplied values.
 * Thread-safe for the use-case of a single writer (web handler) and a single
 * reader (io_task): float writes on Xtensa are atomic at 32-bit alignment.
 * The new values take effect on the next 50 ms EMA window.
 *
 * @param ema_attack     EMA α during playback  (0.005–0.500)
 * @param ema_release    EMA α when stopped     (0.500–2.000)
 * @param stop_thresh    Pause  threshold [RPS] (0.050–0.600)
 * @param start_thresh   Resume threshold [RPS] (0.200–1.200)
 * @param release_ticks  Zero-windows before fast decay (0–10)
 * @param crank_dir      Direction filter: 0=any, +1=positive only, -1=negative only
 */
void encoder2_apply_config(float ema_attack, float ema_release,
                            float stop_thresh, float start_thresh,
                            uint8_t release_ticks, int8_t crank_dir);

#ifdef __cplusplus
}
#endif
