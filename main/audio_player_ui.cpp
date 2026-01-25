#include "audio_player_ui.h"
#include "audio_playback.h"
#include "file_manager_ui.h"
#include "sunton_esp32_8048s050c.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
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
#define NVS_NAMESPACE "audio_player"

// Audio playback state
audio_file_t *audio_files = NULL;  // Allocated on heap (~40KB)
int wav_file_count = 0;
int current_track = -1;
bool is_playing = false;
bool is_paused = false;
TaskHandle_t audio_task_handle = NULL;
i2s_chan_handle_t tx_handle = NULL;
static bool i2s_is_enabled = true;  // Track I2S state (initialized as enabled)
FILE *current_file = NULL;
volatile uint32_t seek_position = 0;  // Byte position to seek to (0 = no seek)
uint32_t wav_data_start_offset = 0;   // Offset in file where WAV data starts
static bool auto_play_enabled = false;       // Auto-play on screen show
bool continue_playback_enabled = false; // Continue to next track when finished
uint8_t volume_level = 80;            // Volume level (0-100), default 80%
uint8_t *file_buffer = NULL;          // Buffer for SD card file reads

// Stats overlay
static lv_obj_t * cpu_label = NULL;
static uint32_t frame_count = 0;
static int64_t last_time = 0;
static int64_t last_transition_time = 0;

// Audio player UI elements
lv_obj_t * title_label = NULL;
lv_obj_t * info_label = NULL;
lv_obj_t * progress_bar = NULL;
lv_obj_t * time_label = NULL;
lv_obj_t * time_remaining_label = NULL;
lv_obj_t * time_total_label = NULL;
static lv_obj_t * autoplay_checkbox = NULL;
static lv_obj_t * continue_checkbox = NULL;
static lv_obj_t * volume_slider = NULL;
static lv_obj_t * audio_player_screen = NULL;

// Control buttons for physical button feedback
static lv_obj_t * btn_prev = NULL;
static lv_obj_t * btn_play = NULL;
static lv_obj_t * btn_pause = NULL;
static lv_obj_t * btn_stop = NULL;
static lv_obj_t * btn_next = NULL;

// Forward declarations
static void btn_prev_event_cb(lv_event_t *e);
static void btn_play_event_cb(lv_event_t *e);
static void btn_pause_event_cb(lv_event_t *e);
static void btn_stop_event_cb(lv_event_t *e);
static void btn_next_event_cb(lv_event_t *e);
static void progress_bar_event_cb(lv_event_t *e);
static void autoplay_checkbox_event_cb(lv_event_t *e);
static void continue_checkbox_event_cb(lv_event_t *e);
static void volume_slider_event_cb(lv_event_t *e);
static void load_audio_config(void);
static void save_audio_config(void);
void set_title_scroll_speed(lv_obj_t *label, const char *text);
void audio_playback_task(void *arg);

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
            
            // Stop playback completely when leaving audio player screen
            audio_player_stop();
            
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
    
    // Create info label (small, below title, shows format info)
    info_label = lv_label_create(screen);
    lv_obj_set_width(info_label, SUNTON_ESP32_LCD_WIDTH - 40);
    lv_obj_align(info_label, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_text_color(info_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_28, 0);
    lv_label_set_text(info_label, "");
    
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
    
    // Create time remaining label (center, below progress bar)
    time_remaining_label = lv_label_create(screen);
    lv_obj_align(time_remaining_label, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_text_color(time_remaining_label, lv_color_hex(0xFF8800), 0);
    lv_obj_set_style_text_font(time_remaining_label, &lv_font_montserrat_48, 0);
    lv_label_set_text(time_remaining_label, "-00:00");
    
    // Create time total label (right side, below progress bar)
    time_total_label = lv_label_create(screen);
    lv_obj_align(time_total_label, LV_ALIGN_TOP_RIGHT, -40, 180);
    lv_obj_set_style_text_color(time_total_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(time_total_label, &lv_font_montserrat_48, 0);
    lv_label_set_text(time_total_label, "00:00");
    
    // Create control buttons (centered below time labels)
    int button_size = 120;
    int button_spacing = 20;
    int total_width = (button_size * 5) + (button_spacing * 4);
    int start_x = (SUNTON_ESP32_LCD_WIDTH - total_width) / 2 - 30;
    int button_y = 260;
    
    // Previous button
    btn_prev = lv_button_create(screen);
    lv_obj_set_size(btn_prev, button_size, button_size);
    lv_obj_set_pos(btn_prev, start_x, button_y);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x666666), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_prev, 20, 0);
    lv_obj_t * label_prev = lv_label_create(btn_prev);
    lv_label_set_text(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &lv_font_montserrat_48, 0);
    lv_obj_center(label_prev);
    lv_obj_add_event_cb(btn_prev, btn_prev_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Play button
    btn_play = lv_button_create(screen);
    lv_obj_set_size(btn_play, button_size, button_size);
    lv_obj_set_pos(btn_play, start_x + button_size + button_spacing, button_y);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x00FF00), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_play, 20, 0);
    lv_obj_t * label_play = lv_label_create(btn_play);
    lv_label_set_text(label_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(label_play, &lv_font_montserrat_48, 0);
    lv_obj_center(label_play);
    lv_obj_add_event_cb(btn_play, btn_play_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Pause button
    btn_pause = lv_button_create(screen);
    lv_obj_set_size(btn_pause, button_size, button_size);
    lv_obj_set_pos(btn_pause, start_x + (button_size + button_spacing) * 2, button_y);
    lv_obj_set_style_bg_color(btn_pause, lv_color_hex(0xAA6600), 0);
    lv_obj_set_style_bg_color(btn_pause, lv_color_hex(0xFF9900), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_pause, 20, 0);
    lv_obj_t * label_pause = lv_label_create(btn_pause);
    lv_label_set_text(label_pause, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(label_pause, &lv_font_montserrat_48, 0);
    lv_obj_center(label_pause);
    lv_obj_add_event_cb(btn_pause, btn_pause_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Stop button
    btn_stop = lv_button_create(screen);
    lv_obj_set_size(btn_stop, button_size, button_size);
    lv_obj_set_pos(btn_stop, start_x + (button_size + button_spacing) * 3, button_y);
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0xAA0000), 0);
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0xFF0000), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_stop, 20, 0);
    lv_obj_t * label_stop = lv_label_create(btn_stop);
    lv_label_set_text(label_stop, LV_SYMBOL_STOP);
    lv_obj_set_style_text_font(label_stop, &lv_font_montserrat_48, 0);
    lv_obj_center(label_stop);
    lv_obj_add_event_cb(btn_stop, btn_stop_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Next button
    btn_next = lv_button_create(screen);
    lv_obj_set_size(btn_next, button_size, button_size);
    lv_obj_set_pos(btn_next, start_x + (button_size + button_spacing) * 4, button_y);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x666666), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_next, 20, 0);
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

lv_obj_t * audio_player_get_time_remaining_label(void)
{
    return time_remaining_label;
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

// Helper function to set scroll speed based on text length
void set_title_scroll_speed(lv_obj_t *label, const char *text) {
    // Get the actual rendered width of the text in pixels
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int32_t text_width = lv_text_get_width(text, strlen(text), font, 0);
    
    // Get label width for reference
    int32_t label_width = lv_obj_get_width(label);
    
    // Calculate scroll duration based on pixel width
    // Aim for constant scroll speed: ~100 pixels per second
    uint32_t duration_ms = (text_width * 1000) / 100;  // 100 pixels/second
    
    // Clamp between 1000ms and 30000ms
    if (duration_ms < 1000) duration_ms = 1000;
    if (duration_ms > 30000) duration_ms = 30000;
    
    lv_obj_set_style_anim_time(label, duration_ms, 0);
    
    // Force restart scroll animation by temporarily disabling and re-enabling scroll mode
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    
    ESP_LOGI(TAG, "Scroll speed: text=\"%s\", text_width=%ld px, label_width=%ld px, duration=%lu ms", 
             text, text_width, label_width, duration_ms);
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

static void btn_stop_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Stop button clicked");
        audio_player_stop();
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
    
    // Update UI with proper locking to prevent watchdog timeout
    lv_lock();
    // Strip file extension from title
    char title_without_ext[MAX_FILENAME_LEN];
    strncpy(title_without_ext, audio->name, MAX_FILENAME_LEN - 1);
    title_without_ext[MAX_FILENAME_LEN - 1] = '\0';
    char *dot = strrchr(title_without_ext, '.');
    if (dot && (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0)) {
        *dot = '\0';
    }
    lv_label_set_text(title_label, title_without_ext);
    set_title_scroll_speed(title_label, title_without_ext);
    
    const char *type_str = audio->type == AUDIO_TYPE_MP3 ? "MP3" : "WAV";
    char info_text[64];
    snprintf(info_text, sizeof(info_text), "%s, %lu Hz, %d ch", 
             type_str, audio->sample_rate, audio->num_channels);
    lv_label_set_text(info_label, info_text);
    
    // Reset progress bar and time
    if (progress_bar) {
        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    }
    if (time_label) {
        lv_label_set_text(time_label, "00:00");
    }
    if (time_remaining_label) {
        lv_label_set_text(time_remaining_label, "-00:00");
    }
    if (time_remaining_label) {
        lv_label_set_text(time_remaining_label, "-00:00");
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
                // Using 128 kbps as default estimate
                uint32_t estimated_bitrate_kbps = 128;
                uint32_t estimated_seconds = (file_size * 8) / (estimated_bitrate_kbps * 1000);
                uint32_t minutes = estimated_seconds / 60;
                uint32_t seconds = estimated_seconds % 60;
                char time_text[16];
                snprintf(time_text, sizeof(time_text), "~%02lu:%02lu", minutes, seconds);
                lv_label_set_text(time_total_label, time_text);
                
                ESP_LOGI(TAG, "MP3 duration estimate: file_size=%ld bytes, bitrate=%lu kbps, duration=%lu:%02lu", 
                         file_size, estimated_bitrate_kbps, minutes, seconds);
            } else {
                lv_label_set_text(time_total_label, "--:--");
                ESP_LOGW(TAG, "Failed to open MP3 file for duration estimation");
            }
        } else {
            lv_label_set_text(time_total_label, "--:--");
        }
    }
    
    lv_unlock();  // Release LVGL lock after all UI updates
    
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
    
    // Strip file extension from title
    char title_without_ext[MAX_FILENAME_LEN];
    strncpy(title_without_ext, audio->name, MAX_FILENAME_LEN - 1);
    title_without_ext[MAX_FILENAME_LEN - 1] = '\0';
    char *dot = strrchr(title_without_ext, '.');
    if (dot && (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0)) {
        *dot = '\0';
    }
    const char *type_str = audio->type == AUDIO_TYPE_MP3 ? "MP3" : "WAV";
    
    // Update UI only
    lv_label_set_text(title_label, title_without_ext);
    set_title_scroll_speed(title_label, title_without_ext);
    
    char info_text[64];
    snprintf(info_text, sizeof(info_text), "%s, %lu Hz, %d ch", 
             type_str, audio->sample_rate, audio->num_channels);
    lv_label_set_text(info_label, info_text);
    
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
    
    // Log the title without extension to match what's displayed
    ESP_LOGI(TAG, "Loaded track: %s (type: %s, %lu Hz, %d ch)", 
             title_without_ext, type_str, audio->sample_rate, audio->num_channels);
}

void audio_player_stop(void)
{
    bool was_playing = is_playing && !is_paused;  // Track if we were actively playing
    
    is_playing = false;
    is_paused = false;
    
    if (audio_task_handle) {
        TaskHandle_t task_copy = audio_task_handle;
        // Wait for task to actually finish (it will set handle to NULL)
        int timeout = 100;  // 100 * 20ms = 2 seconds max
        while (audio_task_handle != NULL && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            timeout--;
        }
        if (audio_task_handle != NULL && task_copy != NULL) {
            // Task didn't exit - check if it's still valid before forcing deletion
            eTaskState state = eTaskGetState(task_copy);
            if (state != eDeleted && state != eInvalid) {
                ESP_LOGW(TAG, "Audio task did not exit in time, forcing termination (state=%d)", state);
                audio_task_handle = NULL;  // Clear handle first to prevent re-entry
                vTaskDelete(task_copy);
            } else {
                ESP_LOGI(TAG, "Audio task already deleted or invalid");
                audio_task_handle = NULL;
            }
        }
    }
    
    // If we were actively playing, flush I2S DMA buffers to prevent old audio bleed
    if (was_playing && tx_handle && i2s_is_enabled) {
        ESP_LOGI(TAG, "Flushing I2S DMA buffers (was actively playing)");
        uint8_t *silence = (uint8_t *)heap_caps_calloc(I2S_BUFFER_SIZE, 1, MALLOC_CAP_DMA);
        if (silence) {
            size_t bytes_written;
            // Write 5 buffers of silence to fully flush DMA chain (8 desc × 1023 frames × 4 bytes = ~32KB)
            // 5 × 8KB = 40KB ensures complete flush
            for (int i = 0; i < 5; i++) {
                i2s_channel_write(tx_handle, silence, I2S_BUFFER_SIZE, &bytes_written, pdMS_TO_TICKS(100));
            }
            free(silence);
            // Wait for silence to propagate through entire audio pipeline
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGI(TAG, "I2S DMA buffers flushed");
        }
    }
    
    // Close file AFTER task has finished to prevent any final read attempts
    if (current_file) {
        fclose(current_file);
        current_file = NULL;
    }
    
    // Disable I2S to stop audio output - but first ensure clean silence to prevent pop
    if (tx_handle && i2s_is_enabled) {
        // Write additional silence to ensure DAC/amplifier are at DC zero before disable
        uint8_t *silence = (uint8_t *)heap_caps_calloc(I2S_BUFFER_SIZE, 1, MALLOC_CAP_DMA);
        if (silence) {
            size_t bytes_written;
            // Write 3 more buffers to ensure clean shutdown
            for (int i = 0; i < 3; i++) {
                i2s_channel_write(tx_handle, silence, I2S_BUFFER_SIZE, &bytes_written, pdMS_TO_TICKS(50));
            }
            free(silence);
        }
        // Wait longer for silence to fully settle in DAC/amplifier before disabling
        vTaskDelay(pdMS_TO_TICKS(100));
        
        i2s_channel_disable(tx_handle);
        i2s_is_enabled = false;
        ESP_LOGI(TAG, "I2S disabled after muting");
    }
    
    // Reset UI elements
    lv_lock();
    if (progress_bar) {
        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    }
    if (time_label) {
        lv_label_set_text(time_label, "00:00");
    }
    lv_unlock();
    
    // Don't reset current_track here - preserve track position for Next/Previous navigation
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

bool audio_player_is_paused(void)
{
    return is_paused;
}

bool audio_player_has_files(void)
{
    return wav_file_count > 0;
}

int audio_player_get_current_track(void)
{
    return current_track;
}

void audio_player_play_current_or_first(void)
{
    if (wav_file_count == 0) {
        ESP_LOGW(TAG, "No audio files to play");
        return;
    }
    
    // If no track selected, start with first track
    int track_to_play = (current_track < 0) ? 0 : current_track;
    
    ESP_LOGI(TAG, "Playing track %d: %s", track_to_play, audio_files[track_to_play].name);
    audio_player_play(audio_files[track_to_play].name);
}

void audio_player_show(void)
{
    // Reset UI if not playing/paused (playback was stopped)
    if (!is_playing && !is_paused) {
        if (time_label) {
            lv_label_set_text(time_label, "00:00");
        }
        if (progress_bar) {
            lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
        }
    }
    
    // Check if auto-play is enabled and not already playing
    if (auto_play_enabled && !is_playing && !is_paused && wav_file_count > 0) {
        ESP_LOGI(TAG, "Auto-play enabled, starting first track");
        audio_player_play(audio_files[0].name);
    }
}

// Timer callback to restore button color after flash
static void button_flash_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *button = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (button) {
        lv_obj_clear_state(button, LV_STATE_PRESSED);
    }
}

// Flash button for visual feedback when physical button is pressed
void audio_player_flash_button(const char *button_name)
{
    lv_obj_t *button = NULL;
    
    if (strcmp(button_name, "play") == 0) {
        button = btn_play;
    } else if (strcmp(button_name, "pause") == 0) {
        button = btn_pause;
    } else if (strcmp(button_name, "stop") == 0) {
        button = btn_stop;
    } else if (strcmp(button_name, "prev") == 0 || strcmp(button_name, "previous") == 0) {
        button = btn_prev;
    } else if (strcmp(button_name, "next") == 0) {
        button = btn_next;
    }
    
    if (button) {
        lv_lock();
        lv_obj_add_state(button, LV_STATE_PRESSED);
        lv_timer_t *timer = lv_timer_create(button_flash_timer_cb, 150, button);
        lv_timer_set_repeat_count(timer, 1);
        lv_unlock();
    }
}
