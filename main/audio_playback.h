#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "lvgl.h"
#include "sunton_esp32_8048s050c.h"

// Audio file types
typedef enum {
    AUDIO_TYPE_WAV,
    AUDIO_TYPE_MP3
} audio_type_t;

// Audio file structure (supports WAV and MP3)
typedef struct {
    char name[64];  // MAX_FILENAME_LEN
    char path[320];  // Increased to accommodate "/sdcard/" + 255 char filename + null
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    uint32_t file_size;  // Total file size (for MP3 seeking)
    audio_type_t type;  // WAV or MP3
} audio_file_t;

// Constants
#define MAX_AUDIO_FILES 50
#define MAX_FILENAME_LEN 64
#define I2S_BUFFER_SIZE 8192  // 8KB I2S DMA buffer
#define SDCARD_BUFFER_SIZE 16384  // 16KB buffer for SD card file reads
#define WAV_HEADER_SIZE 44
#define MP3_BUFFER_SIZE 8192  // Max MP3 frame size (increased for safety)

// Global variables (extern declarations)
extern audio_file_t *audio_files;
extern int wav_file_count;
extern int current_track;
extern bool is_playing;
extern bool is_paused;
extern TaskHandle_t audio_task_handle;
extern i2s_chan_handle_t tx_handle;
extern FILE *current_file;
extern volatile uint32_t seek_position;
extern uint32_t wav_data_start_offset;
extern bool continue_playback_enabled;
extern uint8_t volume_level;
extern uint8_t *file_buffer;

// UI elements
extern lv_obj_t *title_label;
extern lv_obj_t *info_label;
extern lv_obj_t *progress_bar;
extern lv_obj_t *time_label;
extern lv_obj_t *time_total_label;

// Functions
void audio_playback_task(void *arg);
void audio_player_init_i2s(void);
void audio_player_scan_wav_files(void);
bool parse_wav_header(FILE *file, audio_file_t *wav_info);
void set_title_scroll_speed(lv_obj_t *label, const char *text);

// LVGL lock/unlock (need to be implemented in audio_player_ui.cpp or main)
void lv_lock(void);
void lv_unlock(void);

#ifdef __cplusplus
}
#endif
