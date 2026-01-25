#include "audio_playback.h"
#include "audio_player_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>

// minimp3 decoder
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static const char *TAG = "AudioPlayback";

void audio_playback_task(void *arg)
{
    // Subscribe to watchdog to prevent timeout during slow SD card operations
    esp_task_wdt_add(NULL);
    
    uint8_t *buffer = (uint8_t *)heap_caps_malloc(I2S_BUFFER_SIZE, MALLOC_CAP_DMA);
    uint8_t *mp3_buffer = NULL;
    mp3dec_t *mp3_decoder = NULL;  // Allocate on heap, decoder is ~6KB
    
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffer");
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }
    
    // Zero-initialize DMA buffer to prevent old audio data
    memset(buffer, 0, I2S_BUFFER_SIZE);
    
    // Get current track info
    if (current_track < 0 || current_track >= wav_file_count) {
        free(buffer);
        audio_task_handle = NULL;
        esp_task_wdt_delete(NULL);
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
            esp_task_wdt_delete(NULL);
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
            esp_task_wdt_delete(NULL);
            vTaskDelete(NULL);
            return;
        }
        memset(mp3_decoder, 0, sizeof(mp3dec_t));  // Zero to ensure proper mapping
        ESP_LOGI(TAG, "MP3 decoder allocated at %p (size=%d)", mp3_decoder, sizeof(mp3dec_t));
        
        // Allocate PCM buffer from SPIRAM (WiFi uses too much internal RAM)
        size_t pcm_size = MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t) + 64;  // +64 bytes safety
        pcm_buffer = (int16_t *)heap_caps_malloc(pcm_size, MALLOC_CAP_SPIRAM);
        if (!pcm_buffer) {
            ESP_LOGE(TAG, "Failed to allocate PCM buffer from SPIRAM");
            // Try internal RAM as fallback
            pcm_buffer = (int16_t *)heap_caps_malloc(pcm_size, MALLOC_CAP_INTERNAL);
            if (!pcm_buffer) {
                ESP_LOGE(TAG, "Failed to allocate PCM buffer from internal RAM");
                free(mp3_decoder);
                free(mp3_buffer);
                free(buffer);
                esp_task_wdt_delete(NULL);
                audio_task_handle = NULL;
                vTaskDelete(NULL);
                return;
            }
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
            // Reset watchdog while paused to prevent timeout
            esp_task_wdt_reset();
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
        
        // Reset watchdog before potentially slow SD card operations
        esp_task_wdt_reset();
        
        size_t bytes_written = 0;
        
        if (audio->type == AUDIO_TYPE_WAV) {
            // WAV: Simple read and write
            size_t bytes_read = fread(buffer, 1, I2S_BUFFER_SIZE, current_file);
            
            if (bytes_read > 0) {
                // Check if this is the last buffer (near EOF)
                long current_pos = ftell(current_file);
                fseek(current_file, 0, SEEK_END);
                long file_end = ftell(current_file);
                fseek(current_file, current_pos, SEEK_SET);
                
                bool is_last_buffer = (file_end - current_pos) < (long)I2S_BUFFER_SIZE;
                
                // Apply volume scaling (treat buffer as 16-bit samples)
                int16_t *samples = (int16_t *)buffer;
                size_t sample_count = bytes_read / 2;
                
                for (size_t i = 0; i < sample_count; i++) {
                    // Apply volume
                    if (volume_level < 100) {
                        samples[i] = (samples[i] * volume_level) / 100;
                    }
                    
                    // Apply fade-out on last buffer to prevent pop
                    if (is_last_buffer && i > sample_count / 2) {
                        // Fade out the second half of the last buffer
                        int32_t fade = ((sample_count - i) * 1000) / (sample_count / 2);
                        samples[i] = (samples[i] * fade) / 1000;
                    }
                }
                
                i2s_channel_write(tx_handle, buffer, bytes_read, &bytes_written, portMAX_DELAY);
                bytes_played += bytes_written;
            } else {
                // End of WAV file - write silence to keep I2S happy
                memset(buffer, 0, I2S_BUFFER_SIZE);
                i2s_channel_write(tx_handle, buffer, I2S_BUFFER_SIZE, &bytes_written, portMAX_DELAY);
                bytes_written = 0;  // Signal EOF
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
                    // Store bitrate from first valid frame (for seeking and duration calculations)
                    if (!mp3_total_time_updated && frame_info.bitrate_kbps > 0) {
                        mp3_bitrate_kbps = frame_info.bitrate_kbps;
                        ESP_LOGI(TAG, "MP3 bitrate detected: %d kbps (from first frame)", mp3_bitrate_kbps);
                        
                        // Update total time label now that we know the actual bitrate
                        if (time_total_label) {
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
                            ESP_LOGI(TAG, "MP3 duration recalculated: file_size=%lu bytes, bitrate=%d kbps, duration=%02lu:%02lu", 
                                     mp3_file_size, frame_info.bitrate_kbps, minutes, seconds);
                        }
                    }
                    
                    // Update sample rate if changed
                    if (frame_info.hz > 0 && frame_info.hz != (int)audio->sample_rate) {
                        ESP_LOGI(TAG, "MP3 format change detected: %d Hz -> %d Hz, %d ch -> %d ch, bitrate: %d kbps", 
                                 audio->sample_rate, frame_info.hz, audio->num_channels, frame_info.channels, frame_info.bitrate_kbps);
                        
                        audio->sample_rate = frame_info.hz;
                        audio->num_channels = frame_info.channels;
                        bytes_per_second = audio->sample_rate * audio->num_channels * 2;  // 16-bit = 2 bytes
                        
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
                // End of MP3 file - write silence immediately to prevent pop from abrupt ending
                memset(buffer, 0, I2S_BUFFER_SIZE);
                i2s_channel_write(tx_handle, buffer, I2S_BUFFER_SIZE, &bytes_written, portMAX_DELAY);
                ESP_LOGI(TAG, "MP3 buffer empty, file_pos=%lu, file_size=%lu", mp3_file_pos, mp3_file_size);
                bytes_written = 0;  // Signal EOF
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
                
                // Calculate remaining time
                uint32_t total_seconds = 0;
                if (audio->type == AUDIO_TYPE_MP3 && mp3_bitrate_kbps > 0) {
                    total_seconds = (mp3_file_size * 8) / (mp3_bitrate_kbps * 1000);
                } else if (bytes_per_second > 0 && total_bytes > 0) {
                    total_seconds = total_bytes / bytes_per_second;
                }
                uint32_t remaining_seconds = (total_seconds > elapsed_seconds) ? (total_seconds - elapsed_seconds) : 0;
                uint32_t remaining_minutes = remaining_seconds / 60;
                uint32_t remaining_secs = remaining_seconds % 60;
                
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
                lv_obj_t *time_remaining_label = audio_player_get_time_remaining_label();
                if (time_remaining_label) {
                    char time_text[16];
                    snprintf(time_text, sizeof(time_text), "-%02lu:%02lu", remaining_minutes, remaining_secs);
                    lv_label_set_text(time_remaining_label, time_text);
                }
                lv_unlock();
            }
        } else {
            // End of file
            ESP_LOGI(TAG, "Finished playing track");
            
            // Write LOTS of silence immediately to keep I2S fed during entire file transition
            // This prevents I2S DMA underrun pop during file close/open/setup
            uint8_t *end_silence = (uint8_t *)heap_caps_calloc(I2S_BUFFER_SIZE, 1, MALLOC_CAP_DMA);
            if (end_silence) {
                size_t bytes_written;
                // Write MORE to cover the entire file operations duration (10 × 8KB = 80KB)
                for (int i = 0; i < 10; i++) {
                    i2s_channel_write(tx_handle, end_silence, I2S_BUFFER_SIZE, &bytes_written, pdMS_TO_TICKS(50));
                }
                free(end_silence);
            }
            
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
                    // Check if sample rate changed - only reconfigure if needed
                    bool sample_rate_changed = (next_audio->sample_rate != audio->sample_rate);
                    
                    // Update audio pointer
                    audio = next_audio;
                    
                    // Only reconfigure I2S if sample rate changed (avoids disable/enable pop)
                    if (sample_rate_changed) {
                        ESP_LOGI(TAG, "Sample rate changed, reconfiguring I2S");
                        
                        // Mute before disabling I2S to prevent pop - write LOTS of silence
                        uint8_t *silence = (uint8_t *)heap_caps_calloc(I2S_BUFFER_SIZE, 1, MALLOC_CAP_DMA);
                        if (silence) {
                            size_t bytes_written;
                            // Write MORE silence to ensure complete settling
                            for (int i = 0; i < 8; i++) {
                                i2s_channel_write(tx_handle, silence, I2S_BUFFER_SIZE, &bytes_written, pdMS_TO_TICKS(50));
                            }
                            free(silence);
                        }
                        
                        // Wait LONGER for silence to fully settle in DAC and NS4168 amplifier
                        vTaskDelay(pdMS_TO_TICKS(150));
                        
                        // Flush I2S DMA buffers to prevent audio garbage from previous track
                        i2s_channel_disable(tx_handle);
                        vTaskDelay(pdMS_TO_TICKS(100));  // Wait longer for complete shutdown
                        
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
                        
                        // Write MORE silence immediately after enabling to ensure DAC/NS4168 start at zero (prevents pop)
                        uint8_t *startup_silence = (uint8_t *)heap_caps_calloc(I2S_BUFFER_SIZE, 1, MALLOC_CAP_DMA);
                        if (startup_silence) {
                            size_t bytes_written;
                            for (int i = 0; i < 5; i++) {
                                i2s_channel_write(tx_handle, startup_silence, I2S_BUFFER_SIZE, &bytes_written, pdMS_TO_TICKS(50));
                            }
                            free(startup_silence);
                        }
                        
                        // Longer delay to let I2S and NS4168 stabilize after reconfiguration
                        vTaskDelay(pdMS_TO_TICKS(50));
                        
                        ESP_LOGI(TAG, "I2S reconfigured: %lu Hz, %d ch, %d bit", 
                                 audio->sample_rate, audio->num_channels, audio->bits_per_sample);
                    } else {
                        ESP_LOGI(TAG, "Sample rate unchanged (%lu Hz), keeping I2S running", audio->sample_rate);
                        
                        // Even without I2S reconfiguration, write silence to flush previous track
                        // and ensure clean transition (prevents pop from residual audio in DMA buffers)
                        uint8_t *silence = (uint8_t *)heap_caps_calloc(I2S_BUFFER_SIZE, 1, MALLOC_CAP_DMA);
                        if (silence) {
                            size_t bytes_written;
                            // Write enough silence to flush the I2S DMA pipeline
                            for (int i = 0; i < 3; i++) {
                                i2s_channel_write(tx_handle, silence, I2S_BUFFER_SIZE, &bytes_written, pdMS_TO_TICKS(50));
                            }
                            free(silence);
                        }
                        
                        // Brief delay to let silence flush through the audio path
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    
                    // Update UI
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
                    if (progress_bar) {
                        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
                    }
                    if (time_label) {
                        lv_label_set_text(time_label, "00:00");
                    }
                    lv_obj_t *time_remaining_label = audio_player_get_time_remaining_label();
                    if (time_remaining_label) {
                        lv_label_set_text(time_remaining_label, "-00:00");
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
    
    // Don't disable I2S here - let audio_player_stop() manage I2S state
    // This allows stop() to flush DMA buffers before disabling
    
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
    esp_task_wdt_delete(NULL);
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
                size_t name_len = strlen(entry->d_name);
                if (name_len >= MAX_FILENAME_LEN) name_len = MAX_FILENAME_LEN - 1;
                memcpy(audio_files[wav_file_count].name, entry->d_name, name_len);
                audio_files[wav_file_count].name[name_len] = '\0';
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
    lv_lock();  // MUST lock LVGL before updating UI elements
    if (wav_file_count > 0 && title_label) {
        // Set current track to first file so Next/Previous work correctly from startup
        current_track = 0;
        
        // Strip file extension from title
        char title_without_ext[MAX_FILENAME_LEN];
        strncpy(title_without_ext, audio_files[0].name, MAX_FILENAME_LEN - 1);
        title_without_ext[MAX_FILENAME_LEN - 1] = '\0';
        char *dot = strrchr(title_without_ext, '.');
        if (dot && (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0)) {
            *dot = '\0';
        }
        lv_label_set_text(title_label, title_without_ext);
        set_title_scroll_speed(title_label, title_without_ext);
        
        const char *type_str = audio_files[0].type == AUDIO_TYPE_MP3 ? "MP3" : "WAV";
        char info_text[64];
        snprintf(info_text, sizeof(info_text), "%s, %lu Hz, %d ch, %d bit", 
                type_str,
                audio_files[0].sample_rate,
                audio_files[0].num_channels,
                audio_files[0].bits_per_sample);
        lv_label_set_text(info_label, info_text);
        
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
    lv_unlock();
}

// Parse WAV file header
bool parse_wav_header(FILE *file, audio_file_t *wav_info)
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
