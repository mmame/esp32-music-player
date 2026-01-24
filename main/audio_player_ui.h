#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// Initialize the audio player UI
void audio_player_ui_init(lv_display_t * disp);

// Get the audio player screen
lv_obj_t * audio_player_get_screen(void);

// Get UI element references
lv_obj_t * audio_player_get_title_label(void);
lv_obj_t * audio_player_get_progress_bar(void);
lv_obj_t * audio_player_get_time_label(void);
lv_obj_t * audio_player_get_time_total_label(void);
lv_obj_t * audio_player_get_autoplay_checkbox(void);
lv_obj_t * audio_player_get_continue_checkbox(void);

// Audio playback control functions
void audio_player_init_i2s(void);
void audio_player_scan_wav_files(void);
void audio_player_play(const char *filename);
void audio_player_load(const char *filename);  // Load track without playing
void audio_player_stop(void);
void audio_player_pause(void);
void audio_player_resume(void);
void audio_player_next(void);
void audio_player_previous(void);
bool audio_player_is_playing(void);
bool audio_player_is_paused(void);  // Check if playback is paused
bool audio_player_has_files(void);  // Check if there are audio files available
int audio_player_get_current_track(void);  // Get current track index
void audio_player_play_current_or_first(void);  // Play current track or first if none selected
void audio_player_show(void);

#ifdef __cplusplus
}
#endif
