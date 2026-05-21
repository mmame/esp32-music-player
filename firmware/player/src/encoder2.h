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

#ifdef __cplusplus
}
#endif
