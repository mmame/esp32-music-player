#include "audio_player_ui.h"
#include "file_manager_ui.h"
#include "sunton_esp32_8048s050c.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>

static const char *TAG = "AudioPlayer";

#define TRANSITION_IGNORE_MS 300  // Ignore events for 300ms after screen transition
#define MAX_AUDIO_FILES 50
#define MAX_FILENAME_LEN 64
#define I2S_BUFFER_SIZE 8192  // 8KB I2S DMA buffer
#define SDCARD_BUFFER_SIZE 16384  // 16KB buffer for SD card file reads
#define WAV_HEADER_SIZE 44
#define MP3_BUFFER_SIZE 8192  // Max MP3 frame size (increased for safety)
#define NVS_NAMESPACE "audio_player"

// Audio file types
typedef enum {
    AUDIO_TYPE_WAV,
    AUDIO_TYPE_MP3
} audio_type_t;

// Audio file structure (supports WAV and MP3)
typedef struct {
    char name[MAX_FILENAME_LEN];
    char path[320];  // Increased to accommodate "/sdcard/" + 255 char filename + null
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    uint32_t file_size;  // Total file size (for MP3 seeking)
    audio_type_t type;  // WAV or MP3
} audio_file_t;

// minimp3 decoder
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// Audio playback state
static audio_file_t *audio_files = NULL;  // Allocated on heap (~40KB)
static int wav_file_count = 0;
static int current_track = -1;
static bool is_playing = false;
static bool is_paused = false;
static TaskHandle_t audio_task_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static bool i2s_is_enabled = true;  // Track I2S state (initialized as enabled)
static FILE *current_file = NULL;
static volatile uint32_t seek_position = 0;  // Byte position to seek to (0 = no seek)
static uint32_t wav_data_start_offset = 0;   // Offset in file where WAV data starts
static bool auto_play_enabled = false;       // Auto-play on screen show
static bool continue_playback_enabled = false; // Continue to next track when finished
static uint8_t volume_level = 80;            // Volume level (0-100), default 80%
static uint8_t *file_buffer = NULL;          // Buffer for SD card file reads

// Stats overlay
static lv_obj_t * cpu_label = NULL;
static uint32_t frame_count = 0;
static int64_t last_time = 0;
static int64_t last_transition_time = 0;

// Audio player UI elements
static lv_obj_t * title_label = NULL;
static lv_obj_t * progress_bar = NULL;
static lv_obj_t * time_label = NULL;
static lv_obj_t * time_total_label = NULL;
static lv_obj_t * autoplay_checkbox = NULL;
static lv_obj_t * continue_checkbox = NULL;
static lv_obj_t * volume_slider = NULL;
static lv_obj_t * audio_player_screen = NULL;

// Forward declarations
static void btn_prev_event_cb(lv_event_t *e);
static void btn_play_event_cb(lv_event_t *e);
static void btn_pause_event_cb(lv_event_t *e);
static void btn_next_event_cb(lv_event_t *e);
static void progress_bar_event_cb(lv_event_t *e);
static void autoplay_checkbox_event_cb(lv_event_t *e);
static void continue_checkbox_event_cb(lv_event_t *e);
static void volume_slider_event_cb(lv_event_t *e);
static void load_audio_config(void);
static void save_audio_config(void);

// Test: Generate 1kHz sine wave - NS4168 MONO test
static void test_sine_wave_task(void *arg)
{
    const uint32_t sample_rate = 44100;
    const float frequency = 1000.0f;  // 1kHz
    const float amplitude = 0.05f;     // 5% für Test
    const size_t buffer_samples = 1024;
    
    // STEREO buffer - 2 samples per frame (L+R) for NS4168
    int16_t *buffer = (int16_t *)heap_caps_malloc(buffer_samples * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate sine wave buffer");
        vTaskDelete(NULL);
        return;
    }
    
    // Use float phase for proper precision and wrapping
    const float phase_increment = 2.0f * 3.14159265359f * frequency / sample_rate;
    float phase = 0.0f;
    
    ESP_LOGI(TAG, "Generating 1kHz sine wave at 5%% - STEREO for NS4168 (mono output)...");
    
    while (is_playing) {
        if (is_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // STEREO mode - 2 samples per frame (L+R)
        // NS4168 expects stereo input, outputs mono (mixes L+R internally)
        for (size_t i = 0; i < buffer_samples; i++) {
            int16_t sample = (int16_t)(sinf(phase) * 32767.0f * amplitude);
            
            // Fill both channels with same signal (stereo input, mono output)
            buffer[i * 2] = sample;      // Left channel
            buffer[i * 2 + 1] = sample;  // Right channel (NS4168 mixes L+R to mono)
            
            // Increment phase and wrap properly
            phase += phase_increment;
            if (phase >= 6.28318530718f) {  // 2*PI
                phase -= 6.28318530718f;
            }
        }
        
        // Write to I2S - STEREO buffer
        size_t bytes_written;
        i2s_channel_write(tx_handle, (uint8_t*)buffer, buffer_samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    }
    
    // Disable I2S to silence DAC output
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
    }
    
    free(buffer);
    audio_task_handle = NULL;
    ESP_LOGI(TAG, "Sine wave test stopped");
    vTaskDelete(NULL);
}

static void update_stats_timer_cb(lv_timer_t * timer)
{
    // Calculate FPS
    int64_t current_time = esp_timer_get_time();
    float fps = 0;
    
    if (last_time > 0) {
        int64_t elapsed_us = current_time - last_time;
        if (elapsed_us > 0) {
            fps = (float)frame_count * 1000000.0f / (float)elapsed_us;
        }
    }
    
    last_time = current_time;
    frame_count = 0;
    
    // Get CPU usage using heap free memory as a simple metric
    // (More accurate CPU stats would require configGENERATE_RUN_TIME_STATS)
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
    
    // Calculate a simple CPU activity metric based on frame rate
    // Higher FPS typically means higher CPU usage
    float cpu_usage = (fps / 60.0f) * 100.0f;
    if (cpu_usage > 100.0f) cpu_usage = 100.0f;
    
    // Update labels using snprintf for proper float formatting
    char cpu_text[32];
    snprintf(cpu_text, sizeof(cpu_text), "CPU: %.1f%%", cpu_usage);
    
    lv_label_set_text(cpu_label, cpu_text);
}

static void flush_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_FLUSH_FINISH) {
        frame_count++;
    }
}

static void progress_bar_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_PRESSING) {
        if (current_track < 0 || current_track >= wav_file_count) return;
        if (!is_playing && !is_paused) return;
        
        // Get click coordinates
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        
        // Get progress bar coordinates and dimensions
        lv_area_t coords;
        lv_obj_get_coords(progress_bar, &coords);
        
        int32_t bar_width = lv_area_get_width(&coords);
        int32_t bar_left = coords.x1;
        int32_t click_x = point.x - bar_left;
        
        // Calculate percentage (0-100)
        if (click_x < 0) click_x = 0;
        if (click_x > bar_width) click_x = bar_width;
        
        uint32_t percentage = (click_x * 100) / bar_width;
        
        // Calculate byte position in the audio data
        audio_file_t *wav = &audio_files[current_track];
        uint32_t total_size = (wav->type == AUDIO_TYPE_MP3) ? wav->file_size : wav->data_size;
        uint32_t target_byte = (total_size * percentage) / 100;
        
        // Align to sample boundary (important for proper audio playback)
        uint32_t bytes_per_sample = wav->num_channels * (wav->bits_per_sample / 8);
        if (bytes_per_sample > 0) {
            target_byte = (target_byte / bytes_per_sample) * bytes_per_sample;
        }
        
        // Set seek position (will be picked up by playback task)
        seek_position = target_byte;
        
        ESP_LOGI(TAG, "Seek to %lu%% (%lu bytes)", percentage, target_byte);
    }
}

static void screen_gesture_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    // Ignore all events for a short time after screen transition
    int64_t now = esp_timer_get_time() / 1000;  // Convert to ms
    if (now - last_transition_time < TRANSITION_IGNORE_MS) {
        ESP_LOGI("AudioPlayer", "Event ignored - too soon after transition (%lld ms)", now - last_transition_time);
        return;
    }
    
    // Log ALL events to see what's happening
    ESP_LOGI("AudioPlayer", "Event code: %d (PRESSED=%d, RELEASED=%d, GESTURE=%d)", 
             code, LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_GESTURE);
    
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        
        ESP_LOGI("AudioPlayer", "Gesture detected, direction: %d (LEFT=%d, RIGHT=%d, TOP=%d, BOTTOM=%d)", 
                 dir, LV_DIR_LEFT, LV_DIR_RIGHT, LV_DIR_TOP, LV_DIR_BOTTOM);
        
        if (dir == LV_DIR_LEFT) {
            // Swipe left to show file manager
            ESP_LOGI("AudioPlayer", "Swipe LEFT detected, showing file manager");
            last_transition_time = esp_timer_get_time() / 1000;
            file_manager_show();
        }
    }
}

void audio_player_ui_init(lv_display_t * disp)
{
    // Increment frame counter on each flush
    lv_display_add_event_cb(disp, flush_event_cb, LV_EVENT_FLUSH_FINISH, NULL);

    lv_lock();
    
    // Create main screen with black background
    lv_obj_t * screen = lv_screen_active();
    audio_player_screen = screen;  // Store reference
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);  // Disable scroll
    
    // Create song title label (large, scrolling text)
    title_label = lv_label_create(screen);
    lv_obj_set_width(title_label, SUNTON_ESP32_LCD_WIDTH - 40);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_48, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(title_label, "No track loaded");
    // Set constant scroll speed (pixels per second)
    lv_obj_set_style_anim_time(title_label, 5000, 0);
    
    // Create progress bar
    progress_bar = lv_bar_create(screen);
    lv_obj_set_size(progress_bar, SUNTON_ESP32_LCD_WIDTH - 80, 40);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(progress_bar, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(progress_bar, 2, 0);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(progress_bar, progress_bar_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(progress_bar, progress_bar_event_cb, LV_EVENT_PRESSING, NULL);
    
    // Create time elapsed label (left side, below progress bar)
    time_label = lv_label_create(screen);
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 40, 180);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
    lv_label_set_text(time_label, "00:00");
    
    // Create time total label (right side, below progress bar)
    time_total_label = lv_label_create(screen);
    lv_obj_align(time_total_label, LV_ALIGN_TOP_RIGHT, -40, 180);
    lv_obj_set_style_text_color(time_total_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(time_total_label, &lv_font_montserrat_48, 0);
    lv_label_set_text(time_total_label, "00:00");
    
    // Create control buttons (centered below time labels)
    int button_size = 100;
    int button_spacing = 25;
    int total_width = (button_size * 4) + (button_spacing * 3);
    int start_x = (SUNTON_ESP32_LCD_WIDTH - total_width) / 2;
    int button_y = 260;
    
    // Previous button
    lv_obj_t * btn_prev = lv_button_create(screen);
    lv_obj_set_size(btn_prev, button_size, button_size);
    lv_obj_set_pos(btn_prev, start_x, button_y);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(btn_prev, 40, 0);
    lv_obj_t * label_prev = lv_label_create(btn_prev);
    lv_label_set_text(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &lv_font_montserrat_48, 0);
    lv_obj_center(label_prev);
    lv_obj_add_event_cb(btn_prev, btn_prev_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Play button
    lv_obj_t * btn_play = lv_button_create(screen);
    lv_obj_set_size(btn_play, button_size, button_size);
    lv_obj_set_pos(btn_play, start_x + button_size + button_spacing, button_y);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_radius(btn_play, 40, 0);
    lv_obj_t * label_play = lv_label_create(btn_play);
    lv_label_set_text(label_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(label_play, &lv_font_montserrat_48, 0);
    lv_obj_center(label_play);
    lv_obj_add_event_cb(btn_play, btn_play_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Pause button
    lv_obj_t * btn_pause = lv_button_create(screen);
    lv_obj_set_size(btn_pause, button_size, button_size);
    lv_obj_set_pos(btn_pause, start_x + (button_size + button_spacing) * 2, button_y);
    lv_obj_set_style_bg_color(btn_pause, lv_color_hex(0xAA6600), 0);
    lv_obj_set_style_radius(btn_pause, 40, 0);
    lv_obj_t * label_pause = lv_label_create(btn_pause);
    lv_label_set_text(label_pause, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(label_pause, &lv_font_montserrat_48, 0);
    lv_obj_center(label_pause);
    lv_obj_add_event_cb(btn_pause, btn_pause_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Next button
    lv_obj_t * btn_next = lv_button_create(screen);
    lv_obj_set_size(btn_next, button_size, button_size);
    lv_obj_set_pos(btn_next, start_x + (button_size + button_spacing) * 3, button_y);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(btn_next, 40, 0);
    lv_obj_t * label_next = lv_label_create(btn_next);
    lv_label_set_text(label_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(label_next, &lv_font_montserrat_48, 0);
    lv_obj_center(label_next);
    lv_obj_add_event_cb(btn_next, btn_next_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Create Auto-Play checkbox (bottom-left)
    autoplay_checkbox = lv_checkbox_create(screen);
    lv_checkbox_set_text(autoplay_checkbox, "Auto-Play");
    lv_obj_set_style_text_font(autoplay_checkbox, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(autoplay_checkbox, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(autoplay_checkbox, LV_ALIGN_BOTTOM_LEFT, 40, -20);
    lv_obj_set_style_bg_color(autoplay_checkbox, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    lv_obj_add_event_cb(autoplay_checkbox, autoplay_checkbox_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Create Continue Playback checkbox (bottom-center)
    continue_checkbox = lv_checkbox_create(screen);
    lv_checkbox_set_text(continue_checkbox, "Continue Playback");
    lv_obj_set_style_text_font(continue_checkbox, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(continue_checkbox, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(continue_checkbox, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(continue_checkbox, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    lv_obj_add_event_cb(continue_checkbox, continue_checkbox_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Create Volume slider (bottom-right, vertical)
    volume_slider = lv_slider_create(screen);
    lv_obj_set_size(volume_slider, 40, 200);
    lv_obj_align(volume_slider, LV_ALIGN_BOTTOM_RIGHT, -20, -50);  // Moved down (was -80)
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, volume_level, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x444444), 0);  // Same as progress bar
    lv_obj_set_style_bg_opa(volume_slider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(volume_slider, lv_color_hex(0x888888), 0);  // Same as progress bar
    lv_obj_set_style_border_width(volume_slider, 2, 0);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x00FF00), LV_PART_INDICATOR);  // Same as progress bar
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x00FF00), LV_PART_KNOB);
    lv_obj_add_event_cb(volume_slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Create volume icon inside the knob (centered on slider)
    lv_obj_t * volume_label = lv_label_create(volume_slider);
    lv_label_set_text(volume_label, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(volume_label, lv_color_hex(0x000000), 0);  // Black icon on green knob
    lv_obj_center(volume_label);  // Center on slider (will appear on/near knob)
    
    // Create CPU label (bottom-right corner for debugging)
    cpu_label = lv_label_create(screen);
    lv_obj_set_style_text_color(cpu_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_color(cpu_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cpu_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(cpu_label, 4, 0);
    lv_obj_align(cpu_label, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_label_set_text(cpu_label, "CPU: --");
    
    // Load saved settings from NVS
    load_audio_config();
    
    // Set checkbox states based on loaded values
    if (auto_play_enabled) {
        lv_obj_add_state(autoplay_checkbox, LV_STATE_CHECKED);
    }
    if (continue_playback_enabled) {
        lv_obj_add_state(continue_checkbox, LV_STATE_CHECKED);
    }
    
    // Update volume slider with loaded value
    lv_slider_set_value(volume_slider, volume_level, LV_ANIM_OFF);
    
    // Add swipe gesture support to screen
    lv_obj_add_event_cb(screen, screen_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(screen, screen_gesture_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, screen_gesture_event_cb, LV_EVENT_RELEASED, NULL);
    
    // Create timer to update stats every second
    lv_timer_create(update_stats_timer_cb, 1000, NULL);
    
    lv_unlock();
}

// Getter functions
lv_obj_t * audio_player_get_screen(void)
{
    return audio_player_screen;
}

lv_obj_t * audio_player_get_title_label(void)
{
    return title_label;
}

lv_obj_t * audio_player_get_progress_bar(void)
{
    return progress_bar;
}

lv_obj_t * audio_player_get_time_label(void)
{
    return time_label;
}

lv_obj_t * audio_player_get_time_total_label(void)
{
    return time_total_label;
}

lv_obj_t * audio_player_get_autoplay_checkbox(void)
{
    return autoplay_checkbox;
}

lv_obj_t * audio_player_get_continue_checkbox(void)
{
    return continue_checkbox;
}

// Load audio player settings from NVS
static void load_audio_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        uint8_t auto_play = 0;
        nvs_get_u8(nvs_handle, "auto_play", &auto_play);
        auto_play_enabled = (auto_play != 0);
        
        uint8_t continue_play = 0;
        nvs_get_u8(nvs_handle, "continue_play", &continue_play);
        continue_playback_enabled = (continue_play != 0);
        
        nvs_get_u8(nvs_handle, "volume", &volume_level);
        if (volume_level > 100) volume_level = 80;  // Sanity check
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Loaded audio config: auto_play=%d, continue_play=%d, volume=%d", auto_play_enabled, continue_playback_enabled, volume_level);
    } else {
        ESP_LOGI(TAG, "No saved audio config, using defaults");
    }
}

// Save audio player settings to NVS
static void save_audio_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_u8(nvs_handle, "auto_play", auto_play_enabled ? 1 : 0);
        nvs_set_u8(nvs_handle, "continue_play", continue_playback_enabled ? 1 : 0);
        nvs_set_u8(nvs_handle, "volume", volume_level);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Saved audio config");
    }
}

// ========== Checkbox Event Handlers ==========

static void autoplay_checkbox_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *checkbox = (lv_obj_t *)lv_event_get_target(e);
        auto_play_enabled = lv_obj_has_state(checkbox, LV_STATE_CHECKED);
        ESP_LOGI(TAG, "Auto-play %s", auto_play_enabled ? "enabled" : "disabled");
        save_audio_config();
    }
}

static void continue_checkbox_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *checkbox = (lv_obj_t *)lv_event_get_target(e);
        continue_playback_enabled = lv_obj_has_state(checkbox, LV_STATE_CHECKED);
        ESP_LOGI(TAG, "Continue playback %s", continue_playback_enabled ? "enabled" : "disabled");
        save_audio_config();
    }
}

static void volume_slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
        volume_level = lv_slider_get_value(slider);
        ESP_LOGI(TAG, "Volume set to %d%%", volume_level);
        save_audio_config();
    }
}

// ========== Button Event Handlers ==========

static void btn_prev_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Previous button clicked");
        audio_player_previous();
    }
}

static void btn_play_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Play button clicked");
        if (is_playing && !is_paused) {
            ESP_LOGI(TAG, "Already playing");
        } else if (is_paused) {
            audio_player_resume();
        } else {
            // Play audio files (normal mode)
            if (wav_file_count > 0) {
                int track = (current_track >= 0 && current_track < wav_file_count) ? current_track : 0;
                audio_player_play(audio_files[track].name);
            }
            
            // TEST CODE (disabled - uncomment to test sine wave):
            // ESP_LOGI(TAG, "Starting SINE WAVE test (1kHz @ 0.01%)");
            // is_playing = true;
            // is_paused = false;
            // lv_label_set_text(title_label, "TEST: DC+Sine (no zero cross)");
            // xTaskCreate(test_sine_wave_task, "sine_test", 8192, NULL, 10, &audio_task_handle);
        }
    }
}

static void btn_pause_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Pause button clicked");
        if (is_playing && !is_paused) {
            audio_player_pause();
        }
    }
}

static void btn_next_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Next button clicked");
        audio_player_next();
    }
}

// ========== I2S and Audio Playback Implementation ==========

// Parse WAV file header
static bool parse_wav_header(FILE *file, audio_file_t *wav_info)
{
    uint8_t header[WAV_HEADER_SIZE];
    
    if (fread(header, 1, WAV_HEADER_SIZE, file) != WAV_HEADER_SIZE) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        return false;
    }
    
    // Check RIFF header
    if (memcmp(header, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "Invalid RIFF header");
        return false;
    }
    
    // Check WAVE format
    if (memcmp(header + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAVE format");
        return false;
    }
    
    // Check fmt subchunk
    if (memcmp(header + 12, "fmt ", 4) != 0) {
        ESP_LOGE(TAG, "Invalid fmt subchunk");
        return false;
    }
    
    // Parse format data
    uint16_t audio_format = header[20] | (header[21] << 8);
    if (audio_format != 1) {  // PCM
        ESP_LOGE(TAG, "Only PCM format supported");
        return false;
    }
    
    wav_info->num_channels = header[22] | (header[23] << 8);
    wav_info->sample_rate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    wav_info->bits_per_sample = header[34] | (header[35] << 8);
    
    // Find data chunk - it may be beyond the first 44 bytes
    uint32_t fmt_size = header[16] | (header[17] << 8) | (header[18] << 16) | (header[19] << 24);
    uint32_t offset = 20 + fmt_size;  // Start after fmt chunk
    
    // Search for data chunk (read more if needed)
    uint8_t chunk_header[8];
    fseek(file, offset, SEEK_SET);
    
    while (fread(chunk_header, 1, 8, file) == 8) {
        if (memcmp(chunk_header, "data", 4) == 0) {
            wav_info->data_size = chunk_header[4] | (chunk_header[5] << 8) | 
                                 (chunk_header[6] << 16) | (chunk_header[7] << 24);
            // Save data start offset and leave file position at start of data
            wav_data_start_offset = ftell(file);
            return true;
        }
        
        // Skip this chunk and move to next
        uint32_t chunk_size = chunk_header[4] | (chunk_header[5] << 8) | 
                             (chunk_header[6] << 16) | (chunk_header[7] << 24);
        fseek(file, chunk_size, SEEK_CUR);
        offset += 8 + chunk_size;
        
        // Prevent infinite loop
        if (offset > 10000) {
            ESP_LOGE(TAG, "Data chunk not found in first 10KB");
            return false;
        }
    }
    
    ESP_LOGE(TAG, "Data chunk not found");
    return false;
}

// Audio playback task
static void audio_playback_task(void *arg)
{
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(I2S_BUFFER_SIZE, MALLOC_CAP_DMA);
    uint8_t *mp3_buffer = NULL;
    mp3dec_t *mp3_decoder = NULL;  // Allocate on heap, decoder is ~6KB
    
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffer");
        vTaskDelete(NULL);
        return;
    }
    
    // Zero-initialize DMA buffer to prevent old audio data
    memset(buffer, 0, I2S_BUFFER_SIZE);
    
    // Get current track info
    if (current_track < 0 || current_track >= wav_file_count) {
        free(buffer);
        audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    audio_file_t *audio = &audio_files[current_track];
    
    // Allocate PCM buffer on heap (4.5KB, too large for stack)
    int16_t *pcm_buffer = NULL;
    
    // Initialize MP3 decoder if needed
    if (audio->type == AUDIO_TYPE_MP3) {
        ESP_LOGI(TAG, "Allocating MP3 buffers: mp3_buffer=%d, decoder=%d, pcm=%d", 
                 MP3_BUFFER_SIZE, sizeof(mp3dec_t), MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
        
        mp3_buffer = (uint8_t *)heap_caps_malloc(MP3_BUFFER_SIZE, MALLOC_CAP_INTERNAL);
        if (!mp3_buffer) {
            ESP_LOGE(TAG, "Failed to allocate MP3 buffer");
            free(buffer);
            vTaskDelete(NULL);
            return;
        }
        memset(mp3_buffer, 0, MP3_BUFFER_SIZE);  // Zero to ensure proper mapping
        ESP_LOGI(TAG, "MP3 buffer allocated at %p", mp3_buffer);
        
        mp3_decoder = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_INTERNAL);
        if (!mp3_decoder) {
            ESP_LOGE(TAG, "Failed to allocate MP3 decoder (~6KB)");
            free(mp3_buffer);
            free(buffer);
            vTaskDelete(NULL);
            return;
        }
        memset(mp3_decoder, 0, sizeof(mp3dec_t));  // Zero to ensure proper mapping
        ESP_LOGI(TAG, "MP3 decoder allocated at %p (size=%d)", mp3_decoder, sizeof(mp3dec_t));
        
        // Allocate PCM buffer with extra safety margin
        size_t pcm_size = MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t) + 64;  // +64 bytes safety
        pcm_buffer = (int16_t *)heap_caps_malloc(pcm_size, MALLOC_CAP_INTERNAL);
        if (!pcm_buffer) {
            ESP_LOGE(TAG, "Failed to allocate PCM buffer");
            free(mp3_decoder);
            free(mp3_buffer);
            free(buffer);
            vTaskDelete(NULL);
            return;
        }
        memset(pcm_buffer, 0, pcm_size);  // Zero to ensure proper mapping
        ESP_LOGI(TAG, "PCM buffer allocated at %p (size=%d)", pcm_buffer, pcm_size);
        
        // Verify heap integrity before initializing decoder
        if (!heap_caps_check_integrity_all(true)) {
            ESP_LOGE(TAG, "Heap corruption detected BEFORE mp3dec_init");
        }
        
        mp3dec_init(mp3_decoder);
        ESP_LOGI(TAG, "MP3 decoder initialized");
        
        // Verify heap integrity after initializing decoder
        if (!heap_caps_check_integrity_all(true)) {
            ESP_LOGE(TAG, "Heap corruption detected AFTER mp3dec_init");
        }
    }
    
    uint32_t bytes_per_second = audio->sample_rate * audio->num_channels * (audio->bits_per_sample / 8);
    uint32_t total_bytes = audio->data_size;
    uint32_t bytes_played = 0;
    uint32_t last_update_time = 0;
    uint32_t mp3_buffer_pos = 0;
    uint32_t mp3_buffer_len = 0;
    uint32_t mp3_file_pos = 0;  // Track actual file position for MP3
    uint32_t mp3_file_size = 0;  // Total file size for MP3
    bool mp3_total_time_updated = false;  // Track if we've calculated actual MP3 duration
    uint32_t mp3_bitrate_kbps = 192;  // MP3 bitrate (default 192, updated from first frame)
    
    // For MP3 files, get the file size for progress calculation
    if (audio->type == AUDIO_TYPE_MP3) {
        fseek(current_file, 0, SEEK_END);
        mp3_file_size = ftell(current_file);
        fseek(current_file, 0, SEEK_SET);
        total_bytes = mp3_file_size;  // Use file size as total for progress
        ESP_LOGI(TAG, "MP3 file size: %lu bytes", mp3_file_size);
    }
    
    while (is_playing) {
        if (is_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (!current_file) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Handle seek request (simplified for MP3 - seeking is approximate)
        if (seek_position > 0) {
            uint32_t seek_pos = seek_position;
            seek_position = 0;  // Clear seek request
            
            if (audio->type == AUDIO_TYPE_WAV) {
                // Seek to the requested position in the file
                if (fseek(current_file, wav_data_start_offset + seek_pos, SEEK_SET) == 0) {
                    bytes_played = seek_pos;
                    ESP_LOGI(TAG, "Seeked to position %lu bytes", seek_pos);
                } else {
                    ESP_LOGE(TAG, "Seek failed");
                }
            } else if (audio->type == AUDIO_TYPE_MP3) {
                // For MP3, seek directly in file based on percentage
                // seek_pos already represents the desired position in the file
                if (fseek(current_file, seek_pos, SEEK_SET) == 0) {
                    // Reset MP3 decoder state
                    mp3_buffer_pos = 0;
                    mp3_buffer_len = 0;
                    mp3_file_pos = seek_pos;  // Update file position tracker
                    
                    // Calculate elapsed time based on seek position
                    if (bytes_per_second > 0 && mp3_file_size > 0 && mp3_bitrate_kbps > 0) {
                        // Calculate total duration: (file_size * 8 bits) / (bitrate_kbps * 1000)
                        uint32_t total_duration_seconds = (mp3_file_size * 8) / (mp3_bitrate_kbps * 1000);
                        
                        // Calculate elapsed seconds based on seek position percentage
                        uint32_t elapsed_seconds = ((uint64_t)seek_pos * total_duration_seconds) / mp3_file_size;
                        
                        // Convert to PCM bytes for display
                        bytes_played = elapsed_seconds * bytes_per_second;
                    } else {
                        bytes_played = 0;
                    }
                    
                    ESP_LOGD(TAG, "Post-seek state: buffer_pos=%lu, buffer_len=%lu, file_pos=%lu", mp3_buffer_pos, mp3_buffer_len, mp3_file_pos);
                } else {
                    ESP_LOGE(TAG, "MP3 seek failed");
                }
            }
        }
        
        ESP_LOGD(TAG, "After seek check, about to process audio type=%d", audio->type);
        
        size_t bytes_written = 0;
        
        if (audio->type == AUDIO_TYPE_WAV) {
            // WAV: Simple read and write
            size_t bytes_read = fread(buffer, 1, I2S_BUFFER_SIZE, current_file);
            
            if (bytes_read > 0) {
                // Apply volume scaling only if needed (treat buffer as 16-bit samples)
                if (volume_level < 100) {
                    int16_t *samples = (int16_t *)buffer;
                    size_t sample_count = bytes_read / 2;
                    for (size_t i = 0; i < sample_count; i++) {
                        samples[i] = (samples[i] * volume_level) / 100;
                    }
                }
                
                i2s_channel_write(tx_handle, buffer, bytes_read, &bytes_written, portMAX_DELAY);
                bytes_played += bytes_written;
            } else {
                // End of WAV file
                bytes_written = 0;
            }
        } else if (audio->type == AUDIO_TYPE_MP3) {
            // MP3: Decode frames to PCM
            ESP_LOGD(TAG, "MP3 decode loop: buffer_pos=%lu, buffer_len=%lu, file_pos=%lu", mp3_buffer_pos, mp3_buffer_len, mp3_file_pos);
            
            // Read more MP3 data aggressively - when buffer is less than half full
            if (mp3_buffer_len - mp3_buffer_pos < MP3_BUFFER_SIZE / 2) {
                ESP_LOGD(TAG, "MP3 buffer low, will try to read more data");
                if (mp3_buffer_pos > 0) {
                    // Move remaining data to start of buffer
                    memmove(mp3_buffer, mp3_buffer + mp3_buffer_pos, mp3_buffer_len - mp3_buffer_pos);
                    mp3_buffer_len -= mp3_buffer_pos;
                    mp3_buffer_pos = 0;
                }
                
                // Read more data
                size_t bytes_read = fread(mp3_buffer + mp3_buffer_len, 1, 
                                         MP3_BUFFER_SIZE - mp3_buffer_len, current_file);
                ESP_LOGD(TAG, "MP3 read %zu bytes from file (file_pos was %lu, now %lu)", bytes_read, mp3_file_pos, mp3_file_pos + bytes_read);
                if (bytes_read > 0) {
                    mp3_buffer_len += bytes_read;
                    mp3_file_pos += bytes_read;  // Track file position
                }
            }
            
            // Decode one MP3 frame
            if (mp3_buffer_len - mp3_buffer_pos > 0) {
                mp3dec_frame_info_t frame_info;
                int samples = mp3dec_decode_frame(mp3_decoder, mp3_buffer + mp3_buffer_pos,
                                                   mp3_buffer_len - mp3_buffer_pos, pcm_buffer, &frame_info);
                
                ESP_LOGD(TAG, "MP3 decode: samples=%d, frame_bytes=%d, buffer_pos=%lu, buffer_len=%lu, file_pos=%lu", 
                         samples, frame_info.frame_bytes, mp3_buffer_pos, mp3_buffer_len, mp3_file_pos);
                
                if (samples > 0) {
                    // Update sample rate if changed
                    if (frame_info.hz > 0 && frame_info.hz != (int)audio->sample_rate) {
                        audio->sample_rate = frame_info.hz;
                        audio->num_channels = frame_info.channels;
                        bytes_per_second = audio->sample_rate * audio->num_channels * 2;  // 16-bit = 2 bytes
                        
                        // Store bitrate for seeking calculations
                        if (frame_info.bitrate_kbps > 0) {
                            mp3_bitrate_kbps = frame_info.bitrate_kbps;
                        }
                        
                        // Reconfigure I2S completely (clock and slot configuration)
                        ESP_LOGI(TAG, "MP3 format: %d Hz, %d ch", frame_info.hz, frame_info.channels);
                        i2s_channel_disable(tx_handle);
                        
                        // Update clock
                        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(audio->sample_rate);
                        ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg));
                        
                        // Update slot configuration (stereo/mono may have changed)
                        i2s_std_slot_config_t slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(
                            I2S_DATA_BIT_WIDTH_16BIT,
                            I2S_SLOT_MODE_STEREO  // Always stereo for NS4168
                        );
                        ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle, &slot_cfg));
                        
                        i2s_channel_enable(tx_handle);
                        
                        // Update total time label now that we know the format
                        if (!mp3_total_time_updated && time_total_label && frame_info.bitrate_kbps > 0) {
                            // Estimate duration: (file_size_bytes * 8) / (bitrate_kbps * 1000)
                            uint32_t estimated_seconds = (mp3_file_size * 8) / (frame_info.bitrate_kbps * 1000);
                            uint32_t minutes = estimated_seconds / 60;
                            uint32_t seconds = estimated_seconds % 60;
                            char time_text[16];
                            snprintf(time_text, sizeof(time_text), "%02lu:%02lu", minutes, seconds);
                            lv_lock();
                            lv_label_set_text(time_total_label, time_text);
                            lv_unlock();
                            mp3_total_time_updated = true;
                            ESP_LOGI(TAG, "MP3 estimated duration: %02lu:%02lu (bitrate: %d kbps)", minutes, seconds, frame_info.bitrate_kbps);
                        }
                    }
                    
                    // Write PCM samples to I2S
                    // samples = samples per channel (e.g., 1152 for one MP3 frame)
                    // PCM buffer contains interleaved samples, so total = samples * channels
                    size_t pcm_bytes = samples * frame_info.channels * sizeof(int16_t);
                    size_t max_pcm_bytes = MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t);
                    if (pcm_bytes > max_pcm_bytes) {
                        ESP_LOGE(TAG, "PCM overflow detected! samples=%d, channels=%d, pcm_bytes=%d, max=%d", 
                                 samples, frame_info.channels, pcm_bytes, max_pcm_bytes);
                        pcm_bytes = max_pcm_bytes;
                        
                        // Check heap integrity on overflow
                        if (!heap_caps_check_integrity_all(true)) {
                            ESP_LOGE(TAG, "Heap corruption detected during PCM overflow!");
                        }
                    }
                    
                    // Apply volume scaling only if needed
                    if (volume_level < 100) {
                        size_t sample_count = pcm_bytes / 2;
                        for (size_t i = 0; i < sample_count; i++) {
                            pcm_buffer[i] = (pcm_buffer[i] * volume_level) / 100;
                        }
                    }
                    
                    i2s_channel_write(tx_handle, (uint8_t*)pcm_buffer, pcm_bytes, &bytes_written, portMAX_DELAY);
                    bytes_played += bytes_written;
                    
                    // Advance buffer position
                    mp3_buffer_pos += frame_info.frame_bytes;
                } else if (frame_info.frame_bytes > 0) {
                    // Skip invalid frame (likely landed mid-frame after seeking)
                    ESP_LOGW(TAG, "MP3 skipping invalid frame (%d bytes, 0 samples) at buffer_pos=%lu", frame_info.frame_bytes, mp3_buffer_pos);
                    mp3_buffer_pos += frame_info.frame_bytes;
                    bytes_written = 1;  // Keep trying to find valid frames (especially after seeking)
                } else {
                    // No valid frame, read more data
                    if (mp3_buffer_len == mp3_buffer_pos) {
                        // End of file
                        ESP_LOGI(TAG, "MP3 end of file detected (buffer exhausted), file_pos=%lu, file_size=%lu", mp3_file_pos, mp3_file_size);
                        bytes_written = 0;
                    } else {
                        // Skip byte and try again
                        ESP_LOGW(TAG, "MP3 skipping invalid byte at buffer_pos=%lu (remaining=%lu)", mp3_buffer_pos, mp3_buffer_len - mp3_buffer_pos);
                        mp3_buffer_pos++;
                        bytes_written = 1;  // Keep trying
                    }
                }
            } else {
                // End of MP3 file
                ESP_LOGI(TAG, "MP3 buffer empty, file_pos=%lu, file_size=%lu", mp3_file_pos, mp3_file_size);
                bytes_written = 0;
            }
        }
        
        // Check if we wrote any data
        ESP_LOGD(TAG, "Checking bytes_written: %zu (type=%d)", bytes_written, audio->type);
        if (bytes_written > 0) {
            // Update UI every 500ms to minimize lock contention
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - last_update_time >= 500) {
                last_update_time = now;
                
                // Calculate progress
                uint32_t progress = 0;
                if (audio->type == AUDIO_TYPE_MP3 && mp3_file_size > 0) {
                    // For MP3, use file position vs file size
                    progress = (mp3_file_pos * 100) / mp3_file_size;
                } else if (total_bytes > 0) {
                    // For WAV, use bytes played vs total bytes
                    progress = (bytes_played * 100) / total_bytes;
                }
                if (progress > 100) progress = 100;
                
                // Calculate elapsed time (based on PCM bytes output)
                uint32_t elapsed_seconds = 0;
                if (bytes_per_second > 0) {
                    elapsed_seconds = bytes_played / bytes_per_second;
                }
                uint32_t minutes = elapsed_seconds / 60;
                uint32_t seconds = elapsed_seconds % 60;
                
                // Update UI with LVGL lock - keep it brief
                lv_lock();
                if (progress_bar) {
                    lv_bar_set_value(progress_bar, progress, LV_ANIM_OFF);
                }
                if (time_label) {
                    char time_text[16];
                    snprintf(time_text, sizeof(time_text), "%02lu:%02lu", minutes, seconds);
                    lv_label_set_text(time_label, time_text);
                }
                lv_unlock();
            }
        } else {
            // End of file
            ESP_LOGI(TAG, "Finished playing track");
            
            // Update UI to show 100% completion
            lv_lock();
            if (progress_bar) {
                lv_bar_set_value(progress_bar, 100, LV_ANIM_OFF);
            }
            lv_unlock();
            
            // Check if we should continue to next track
            if (continue_playback_enabled && wav_file_count > 0) {
                int next_track = (current_track + 1) % wav_file_count;
                ESP_LOGI(TAG, "Continue playback: playing next track %d", next_track);
                
                // Close current file
                if (current_file) {
                    fclose(current_file);
                    current_file = NULL;
                }
                
                // Open next track
                current_track = next_track;
                audio_file_t *next_audio = &audio_files[current_track];
                current_file = fopen(next_audio->path, "rb");
                
                // Enable full buffering for SD card reads
                if (current_file) {
                    setvbuf(current_file, (char*)file_buffer, _IOFBF, SDCARD_BUFFER_SIZE);
                }
                
                bool next_ok = false;
                if (current_file) {
                    if (next_audio->type == AUDIO_TYPE_WAV) {
                        // Switching to WAV - free MP3 buffers if they exist
                        if (mp3_buffer) {
                            free(mp3_buffer);
                            mp3_buffer = NULL;
                        }
                        if (mp3_decoder) {
                            free(mp3_decoder);
                            mp3_decoder = NULL;
                        }
                        if (pcm_buffer) {
                            free(pcm_buffer);
                            pcm_buffer = NULL;
                        }
                        ESP_LOGI(TAG, "Freed MP3 buffers, switching to WAV");
                        
                        next_ok = parse_wav_header(current_file, next_audio);
                    } else if (next_audio->type == AUDIO_TYPE_MP3) {
                        // Switching to MP3 - ensure fresh buffers
                        ESP_LOGI(TAG, "Switching to MP3, allocating fresh buffers");
                        
                        // Free old buffers if they exist
                        if (mp3_buffer) free(mp3_buffer);
                        if (mp3_decoder) free(mp3_decoder);
                        if (pcm_buffer) free(pcm_buffer);
                        
                        // Allocate fresh MP3 buffers
                        mp3_decoder = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_INTERNAL);
                        mp3_buffer = (uint8_t *)heap_caps_malloc(MP3_BUFFER_SIZE, MALLOC_CAP_INTERNAL);
                        pcm_buffer = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
                        
                        if (mp3_decoder && mp3_buffer && pcm_buffer) {
                            // Zero buffers for clean state
                            memset(mp3_buffer, 0, MP3_BUFFER_SIZE);
                            memset(mp3_decoder, 0, sizeof(mp3dec_t));
                            memset(pcm_buffer, 0, MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t));
                            
                            mp3dec_init(mp3_decoder);
                            mp3_buffer_pos = 0;
                            mp3_buffer_len = 0;
                            mp3_file_pos = 0;
                            mp3_total_time_updated = false;
                            // Get file size for new MP3 track
                            fseek(current_file, 0, SEEK_END);
                            mp3_file_size = ftell(current_file);
                            fseek(current_file, 0, SEEK_SET);
                            next_ok = true;
                        } else {
                            ESP_LOGE(TAG, "Failed to allocate MP3 buffers for next track");
                            next_ok = false;
                        }
                    }
                }
                
                if (next_ok) {
                    // Update audio pointer
                    audio = next_audio;
                    
                    // Flush I2S DMA buffers to prevent audio garbage from previous track
                    i2s_channel_disable(tx_handle);
                    vTaskDelay(pdMS_TO_TICKS(50));  // Wait for DMA to fully stop
                    
                    // Update clock configuration
                    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(audio->sample_rate);
                    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg));
                    
                    // Update slot configuration (stereo/mono may have changed between MP3 and WAV)
                    i2s_std_slot_config_t slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, 
                        I2S_SLOT_MODE_STEREO  // Always stereo for NS4168
                    );
                    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle, &slot_cfg));
                    
                    i2s_channel_enable(tx_handle);
                    
                    // Small delay to let I2S stabilize after reconfiguration
                    vTaskDelay(pdMS_TO_TICKS(10));
                    
                    ESP_LOGI(TAG, "I2S reconfigured: %lu Hz, %d ch, %d bit", 
                             audio->sample_rate, audio->num_channels, audio->bits_per_sample);
                    
                    // Update UI
                    lv_lock();
                    const char *type_str = audio->type == AUDIO_TYPE_MP3 ? "MP3" : "WAV";
                    char title_text[128];
                    snprintf(title_text, sizeof(title_text), "%s (%s)", audio->name, type_str);
                    lv_label_set_text(title_label, title_text);
                    if (progress_bar) {
                        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
                    }
                    if (time_label) {
                        lv_label_set_text(time_label, "00:00");
                    }
                    if (time_total_label) {
                        if (audio->type == AUDIO_TYPE_WAV && audio->sample_rate > 0 && audio->data_size > 0) {
                            uint32_t total_seconds = audio->data_size / 
                                (audio->sample_rate * audio->num_channels * (audio->bits_per_sample / 8));
                            uint32_t minutes = total_seconds / 60;
                            uint32_t seconds = total_seconds % 60;
                            char time_text[16];
                            snprintf(time_text, sizeof(time_text), "%02lu:%02lu", minutes, seconds);
                            lv_label_set_text(time_total_label, time_text);
                        } else {
                            lv_label_set_text(time_total_label, "--:--");
                        }
                    }
                    lv_unlock();
                    
                    // Reset playback state for new track
                    bytes_per_second = audio->sample_rate * audio->num_channels * (audio->bits_per_sample / 8);
                    total_bytes = audio->type == AUDIO_TYPE_MP3 ? mp3_file_size : audio->data_size;
                    bytes_played = 0;
                    seek_position = 0;
                    
                    ESP_LOGI(TAG, "Started playing next track: %s (%s)", audio->name, type_str);
                    continue;  // Continue playing
                } else {
                    ESP_LOGE(TAG, "Failed to open next track");
                    if (current_file) {
                        fclose(current_file);
                        current_file = NULL;
                    }
                }
            }
            
            is_playing = false;
            break;
        }
    }
    
    // Disable I2S to silence DAC output
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
    }
    
    // Zero all buffers before freeing to ensure no data leaks to next task
    memset(buffer, 0, I2S_BUFFER_SIZE);
    free(buffer);
    
    if (mp3_buffer) {
        memset(mp3_buffer, 0, MP3_BUFFER_SIZE);
        free(mp3_buffer);
    }
    if (mp3_decoder) {
        memset(mp3_decoder, 0, sizeof(mp3dec_t));
        free(mp3_decoder);
    }
    if (pcm_buffer) {
        size_t pcm_size = MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t) + 64;
        memset(pcm_buffer, 0, pcm_size);
        free(pcm_buffer);
    }
    
    audio_task_handle = NULL;
    vTaskDelete(NULL);
}

// Initialize I2S
void audio_player_init_i2s(void)
{
    ESP_LOGI(TAG, "Initializing I2S...");
    
    // Increase DMA buffer count and size for smoother audio
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,      // Increased from default 6
        .dma_frame_num = 1023,   // Maximum allowed (was 1024, caused warning)
        .auto_clear = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SUNTON_ESP32_I2S_BCLK,
            .ws = SUNTON_ESP32_I2S_LRCLK,
            .dout = SUNTON_ESP32_I2S_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    
    ESP_LOGI(TAG, "I2S initialized successfully");
}

// Scan for WAV files on SD card
void audio_player_scan_wav_files(void)
{
    ESP_LOGI(TAG, "Scanning for audio files (WAV/MP3)...");
    
    // Allocate audio files array on heap if not already allocated
    if (!audio_files) {
        audio_files = (audio_file_t *)heap_caps_malloc(MAX_AUDIO_FILES * sizeof(audio_file_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!audio_files) {
            ESP_LOGE(TAG, "Failed to allocate audio files array (~40KB) from PSRAM");
            return;
        }
        memset(audio_files, 0, MAX_AUDIO_FILES * sizeof(audio_file_t));  // Zero to ensure proper mapping
        ESP_LOGI(TAG, "Allocated %d KB for audio files array from internal RAM", (MAX_AUDIO_FILES * sizeof(audio_file_t)) / 1024);
    }
    
    wav_file_count = 0;
    
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open SD card directory");
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && wav_file_count < MAX_AUDIO_FILES) {
        if (entry->d_type == DT_REG) {
            // Check if file has .wav or .mp3 extension
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0)) {
                strncpy(audio_files[wav_file_count].name, entry->d_name, MAX_FILENAME_LEN - 1);
                snprintf(audio_files[wav_file_count].path, sizeof(audio_files[wav_file_count].path), 
                        "/sdcard/%s", entry->d_name);
                
                // Set file type
                if (strcasecmp(ext, ".mp3") == 0) {
                    audio_files[wav_file_count].type = AUDIO_TYPE_MP3;
                    // MP3 files will be parsed during playback
                    audio_files[wav_file_count].sample_rate = 44100;  // Default, will be updated
                    audio_files[wav_file_count].num_channels = 2;
                    audio_files[wav_file_count].bits_per_sample = 16;
                    
                    // Get file size for MP3 seeking
                    struct stat st;
                    if (stat(audio_files[wav_file_count].path, &st) == 0) {
                        audio_files[wav_file_count].file_size = st.st_size;
                    } else {
                        audio_files[wav_file_count].file_size = 0;
                    }
                    
                    ESP_LOGI(TAG, "Found MP3 file: %s (%lu bytes)", audio_files[wav_file_count].name, audio_files[wav_file_count].file_size);
                } else {
                    audio_files[wav_file_count].type = AUDIO_TYPE_WAV;
                    // Try to parse WAV header for file info
                    FILE *f = fopen(audio_files[wav_file_count].path, "rb");
                    if (f) {
                        parse_wav_header(f, &audio_files[wav_file_count]);
                        fclose(f);
                        
                        // Get file size
                        struct stat st;
                        if (stat(audio_files[wav_file_count].path, &st) == 0) {
                            audio_files[wav_file_count].file_size = st.st_size;
                        } else {
                            audio_files[wav_file_count].file_size = 0;
                        }
                    }
                    ESP_LOGI(TAG, "Found WAV file: %s (%lu Hz, %d ch, %d bit)", 
                            audio_files[wav_file_count].name,
                            audio_files[wav_file_count].sample_rate,
                            audio_files[wav_file_count].num_channels,
                            audio_files[wav_file_count].bits_per_sample);
                }
                
                wav_file_count++;
                
                // Yield to prevent watchdog timeout with many files
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
    
    closedir(dir);
    ESP_LOGI(TAG, "Found %d audio files", wav_file_count);
    
    // Sort files alphabetically/numerically
    if (wav_file_count > 1) {
        for (int i = 0; i < wav_file_count - 1; i++) {
            for (int j = i + 1; j < wav_file_count; j++) {
                if (strcasecmp(audio_files[i].name, audio_files[j].name) > 0) {
                    // Swap
                    audio_file_t temp = audio_files[i];
                    audio_files[i] = audio_files[j];
                    audio_files[j] = temp;
                }
            }
            // Yield every few iterations to prevent watchdog timeout
            if (i % 10 == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        ESP_LOGI(TAG, "Sorted audio files alphabetically");
    }
    
    // Update UI with first file if available
    if (wav_file_count > 0 && title_label) {
        char info_text[128];
        const char *type_str = audio_files[0].type == AUDIO_TYPE_MP3 ? "MP3" : "WAV";
        snprintf(info_text, sizeof(info_text), "%s (%s, %lu Hz, %d ch, %d bit)", 
                audio_files[0].name,
                type_str,
                audio_files[0].sample_rate,
                audio_files[0].num_channels,
                audio_files[0].bits_per_sample);
        lv_label_set_text(title_label, info_text);
        
        // Calculate and display total time (for WAV files with known data size)
        if (audio_files[0].type == AUDIO_TYPE_WAV && audio_files[0].sample_rate > 0 && audio_files[0].data_size > 0) {
            uint32_t total_seconds = audio_files[0].data_size / 
                (audio_files[0].sample_rate * audio_files[0].num_channels * (audio_files[0].bits_per_sample / 8));
            uint32_t minutes = total_seconds / 60;
            uint32_t seconds = total_seconds % 60;
            char time_text[16];
            snprintf(time_text, sizeof(time_text), "%02lu:%02lu", minutes, seconds);
            lv_label_set_text(time_total_label, time_text);
        } else if (audio_files[0].type == AUDIO_TYPE_MP3) {
            lv_label_set_text(time_total_label, "--:--");  // MP3 duration calculated during playback
        }
    } else if (title_label) {
        lv_label_set_text(title_label, "No audio files found on SD card");
    }
}

// Play a WAV file
void audio_player_play(const char *filename)
{
    audio_player_stop();
    
    // Find the file in our list
    int track_idx = -1;
    for (int i = 0; i < wav_file_count; i++) {
        if (strcmp(audio_files[i].name, filename) == 0) {
            track_idx = i;
            break;
        }
    }
    
    if (track_idx < 0) {
        ESP_LOGE(TAG, "File not found in playlist: %s", filename);
        return;
    }
    
    current_track = track_idx;
    audio_file_t *audio = &audio_files[track_idx];
    
    current_file = fopen(audio->path, "rb");
    if (!current_file) {
        ESP_LOGE(TAG, "Failed to open file: %s", audio->path);
        return;
    }
    
    // Allocate FRESH file_buffer BEFORE setvbuf to prevent using old cached data
    if (!file_buffer) {
        file_buffer = (uint8_t *)heap_caps_malloc(SDCARD_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!file_buffer) {
            ESP_LOGE(TAG, "Failed to allocate file buffer");
            fclose(current_file);
            current_file = NULL;
            return;
        }
    }
    memset(file_buffer, 0, SDCARD_BUFFER_SIZE);  // Clear it
    
    // Enable full buffering with OUR buffer to prevent setvbuf from using cached data
    setvbuf(current_file, (char*)file_buffer, _IOFBF, SDCARD_BUFFER_SIZE);
    ESP_LOGI(TAG, "File buffering enabled: %d bytes", SDCARD_BUFFER_SIZE);
    
    // Parse header for WAV files only
    if (audio->type == AUDIO_TYPE_WAV) {
        if (!parse_wav_header(current_file, audio)) {
            fclose(current_file);
            current_file = NULL;
            return;
        }
    }
    
    // Reconfigure I2S for this file's format (already disabled by audio_player_stop)
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(audio->sample_rate);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg));
    
    // Reconfigure slot configuration (ensures proper stereo/mono handling)
    i2s_std_slot_config_t slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO  // Always stereo for NS4168
    );
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle, &slot_cfg));
    
    i2s_channel_enable(tx_handle);
    i2s_is_enabled = true;
    
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Update UI
    const char *type_str = audio->type == AUDIO_TYPE_MP3 ? "MP3" : "WAV";
    char title_text[128];
    snprintf(title_text, sizeof(title_text), "%s (%s, %lu Hz, %d ch)", 
             audio->name, type_str, audio->sample_rate, audio->num_channels);
    lv_label_set_text(title_label, title_text);
    
    // Reset progress bar and time
    if (progress_bar) {
        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    }
    if (time_label) {
        lv_label_set_text(time_label, "00:00");
    }
    
    // Update total time
    if (time_total_label) {
        if (audio->type == AUDIO_TYPE_WAV && audio->sample_rate > 0 && audio->data_size > 0) {
            uint32_t total_seconds = audio->data_size / 
                (audio->sample_rate * audio->num_channels * (audio->bits_per_sample / 8));
            uint32_t minutes = total_seconds / 60;
            uint32_t seconds = total_seconds % 60;
            char time_text[16];
            snprintf(time_text, sizeof(time_text), "%02lu:%02lu", minutes, seconds);
            lv_label_set_text(time_total_label, time_text);
        } else if (audio->type == AUDIO_TYPE_MP3) {
            // Estimate MP3 duration assuming 192 kbps average bitrate
            FILE *f = fopen(audio->path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fclose(f);
                
                // duration = (file_size_bytes * 8) / (bitrate_bps)
                // Using 192 kbps as default estimate
                uint32_t estimated_seconds = (file_size * 8) / (192 * 1000);
                uint32_t minutes = estimated_seconds / 60;
                uint32_t seconds = estimated_seconds % 60;
                char time_text[16];
                snprintf(time_text, sizeof(time_text), "~%02lu:%02lu", minutes, seconds);
                lv_label_set_text(time_total_label, time_text);
            } else {
                lv_label_set_text(time_total_label, "--:--");
            }
        } else {
            lv_label_set_text(time_total_label, "--:--");
        }
    }
    
    // Start playback
    is_playing = true;
    is_paused = false;
    seek_position = 0;  // Clear any pending seek
    
    // Larger stack for MP3 decoding + bigger buffers
    // Higher priority (10) to minimize audio glitches
    xTaskCreate(audio_playback_task, "audio_task", 20480, NULL, 10, &audio_task_handle);
    
    ESP_LOGI(TAG, "Started playing: %s", filename);
}

// Load a track and update UI but don't start playback
void audio_player_load(const char *filename)
{
    audio_player_stop();
    
    // Find the file in our list
    int track_idx = -1;
    for (int i = 0; i < wav_file_count; i++) {
        if (strcmp(audio_files[i].name, filename) == 0) {
            track_idx = i;
            break;
        }
    }
    
    if (track_idx < 0) {
        ESP_LOGE(TAG, "File not found in playlist: %s", filename);
        return;
    }
    
    current_track = track_idx;
    audio_file_t *audio = &audio_files[track_idx];
    
    // Update UI only
    const char *type_str = audio->type == AUDIO_TYPE_MP3 ? "MP3" : "WAV";
    char title_text[128];
    snprintf(title_text, sizeof(title_text), "%s (%s, %lu Hz, %d ch)", 
             audio->name, type_str, audio->sample_rate, audio->num_channels);
    lv_label_set_text(title_label, title_text);
    
    // Reset progress bar and time
    if (progress_bar) {
        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    }
    if (time_label) {
        lv_label_set_text(time_label, "00:00");
    }
    
    // Update total time
    if (time_total_label) {
        if (audio->type == AUDIO_TYPE_WAV && audio->sample_rate > 0 && audio->data_size > 0) {
            uint32_t total_seconds = audio->data_size / 
                (audio->sample_rate * audio->num_channels * (audio->bits_per_sample / 8));
            uint32_t minutes = total_seconds / 60;
            uint32_t seconds = total_seconds % 60;
            char time_text[16];
            snprintf(time_text, sizeof(time_text), "%02lu:%02lu", minutes, seconds);
            lv_label_set_text(time_total_label, time_text);
        } else if (audio->type == AUDIO_TYPE_MP3) {
            // Estimate MP3 duration assuming 192 kbps average bitrate
            FILE *f = fopen(audio->path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fclose(f);
                
                // duration = (file_size_bytes * 8) / (bitrate_bps)
                uint32_t estimated_seconds = (file_size * 8) / (192 * 1000);
                uint32_t minutes = estimated_seconds / 60;
                uint32_t seconds = estimated_seconds % 60;
                char time_text[16];
                snprintf(time_text, sizeof(time_text), "~%02lu:%02lu", minutes, seconds);
                lv_label_set_text(time_total_label, time_text);
            } else {
                lv_label_set_text(time_total_label, "--:--");
            }
        }
    }
    
    ESP_LOGI(TAG, "Loaded track: %s", filename);
}

void audio_player_stop(void)
{
    is_playing = false;
    is_paused = false;
    
    if (audio_task_handle) {
        // Wait for task to actually finish (it will set handle to NULL)
        int timeout = 100;  // 100 * 20ms = 2 seconds max
        while (audio_task_handle != NULL && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            timeout--;
        }
        if (audio_task_handle != NULL) {
            ESP_LOGW(TAG, "Audio task did not exit in time, forcing termination");
            vTaskDelete(audio_task_handle);
            audio_task_handle = NULL;
        }
    }
    
    // Close file AFTER task has finished to prevent any final read attempts
    if (current_file) {
        fclose(current_file);
        current_file = NULL;
    }
    
    // Disable I2S to stop audio output
    if (tx_handle && i2s_is_enabled) {
        i2s_channel_disable(tx_handle);
        i2s_is_enabled = false;
    }
    
    current_track = -1;
}

void audio_player_pause(void)
{
    is_paused = true;
}

void audio_player_resume(void)
{
    is_paused = false;
}

void audio_player_next(void)
{
    if (wav_file_count == 0) return;
    
    // Clear seek position to prevent issues with buffer state
    seek_position = 0;
    
    int next_track = (current_track + 1) % wav_file_count;
    
    // Only play if auto-play is enabled, otherwise just load
    if (auto_play_enabled) {
        audio_player_play(audio_files[next_track].name);
    } else {
        audio_player_load(audio_files[next_track].name);
    }
}

void audio_player_previous(void)
{
    if (wav_file_count == 0) return;
    
    // Clear seek position to prevent issues with buffer state
    seek_position = 0;
    
    int prev_track = (current_track - 1 + wav_file_count) % wav_file_count;
    
    // Only play if auto-play is enabled, otherwise just load
    if (auto_play_enabled) {
        audio_player_play(audio_files[prev_track].name);
    } else {
        audio_player_load(audio_files[prev_track].name);
    }
}

bool audio_player_is_playing(void)
{
    return is_playing && !is_paused;
}

void audio_player_show(void)
{
    // Check if auto-play is enabled and not already playing
    if (auto_play_enabled && !is_playing && !is_paused && wav_file_count > 0) {
        ESP_LOGI(TAG, "Auto-play enabled, starting first track");
        audio_player_play(audio_files[0].name);
    }
}
