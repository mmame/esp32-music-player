/**
 * @file main.cpp
 * @brief WAV player – Master controller.
 *
 * Architecture
 * ────────────
 * Core 1 (high priority)  – audio_task: player.copy() loop
 * Core 0 (medium priority) – io_task:   UART rx/tx, encoder, potentiometers
 *
 * Audio pipeline (write direction):
 *   SD card → PSRAMTaskStream (512 KB PSRAM ring buffer) → WAVDecoder → SonicStream (WSOLA) → I2SStream → DAC
 *
 * UART link (921600 baud, UART1):
 *   TX: CMD_SONG_LIST, CMD_SET_STATE, CMD_POTI_UPDATE, CMD_ENCODER_MOVE,
 *       CMD_ENCODER_BTN, CMD_SYNC
 *   RX: CMD_PLAY_SONG, CMD_STOP_SONG, CMD_ACK
 */

#include "compat.h"           // ESP-IDF 5.x shims — must precede AudioTools
#include "AudioTools.h"
#include "AudioTools/Disk/AudioSourceSD.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"
#include "AudioTools/Communication/HTTP/URLStreamBufferedT.h"
#include "SonicStream.h"
#include "uart_master.h"
#include "encoder.h"
#include "potis.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <atomic>

#include <SD.h>
#include <SPI.h>
#include <string.h>
#include <math.h>
#include "pins.h"
#include "esp_pm.h"


static const char *TAG = "main";


// ─── Playlist ────────────────────────────────────────────────────────────────
#define MAX_SONGS  128
#define MAX_NAME   64    // must match UM_MAX_SONG_NAME

static char     g_song_names[MAX_SONGS][MAX_NAME];
static uint8_t  g_song_count   = 0;
static int16_t  g_current_song = -1;  // index into g_song_names, -1 = none

// ─── PSRAM-backed buffered SD stream ─────────────────────────────────────────
// A 512 KB ring buffer sits in PSRAM between the SD File and the audio pipeline.
// A FreeRTOS fill-task on Core 0 reads SD → PSRAM; audio_task on Core 1 reads
// purely from PSRAM.  Lock-free single-producer/single-consumer design:
//   head_ is owned exclusively by the consumer (Core 1)
//   tail_ is owned exclusively by the producer (Core 0)
class PSRAMTaskStream : public AudioStream {
public:
    static constexpr size_t BUF_SIZE   = 1024UL * 1024;  // 1 MB ring buffer
    static constexpr size_t FILL_CHUNK = 8192;           // SD → PSRAM read size

    PSRAMTaskStream() = default;
    ~PSRAMTaskStream() {
        terminate();
        if (buf_) { heap_caps_free(buf_); buf_ = nullptr; }
    }

    void setFile(File *f) { p_file_ = f; }

    // Allocate PSRAM (once) and start the fill task on Core 0.
    bool begin() {
        if (!buf_) {
            // Allocate ring buffer + SD read temp buffer together in PSRAM.
            buf_ = static_cast<uint8_t *>(
                heap_caps_malloc(BUF_SIZE + FILL_CHUNK,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!buf_) {
                ESP_LOGE("PSRAMBuf", "PSRAM alloc failed (%zu KB)",
                         (BUF_SIZE + FILL_CHUNK) / 1024);
                return false;
            }
            tmp_buf_ = buf_ + BUF_SIZE;   // temp buffer after the ring buffer
            ESP_LOGI("PSRAMBuf", "PSRAM ring buffer: %zu KB @ %p",
                     BUF_SIZE / 1024, buf_);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        eof_.store(false,  std::memory_order_relaxed);
        stop_.store(false, std::memory_order_relaxed);
        xTaskCreatePinnedToCore(fill_task_fn, "sd_fill",
                                8192,   // stack: no large locals, but File I/O
                                this,
                                configMAX_PRIORITIES - 2,
                                &task_,
                                0 /* Core 0 */);
        return true;
    }

    // Stop fill task and reset ring buffer for the next song.
    void end() {
        terminate();
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        eof_.store(false, std::memory_order_relaxed);
    }

    // Seek the underlying SD file to byte_offset, flush the ring buffer, and
    // restart the fill task.  Called from audio_task (Core 1) only.
    bool seekFile(uint32_t byte_offset) {
        if (!p_file_) return false;
        terminate();   // stop fill task
        bool ok = p_file_->seek(byte_offset);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        eof_.store(false,  std::memory_order_relaxed);
        stop_.store(false, std::memory_order_relaxed);
        xTaskCreatePinnedToCore(fill_task_fn, "sd_fill",
                                8192, this,
                                configMAX_PRIORITIES - 2,
                                &task_, 0 /* Core 0 */);
        return ok;
    }

    // ── Stream interface – called from audio_task (Core 1) ───────────────────
    int available() override {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (int)((t - h + BUF_SIZE) % BUF_SIZE);
    }

    size_t readBytes(uint8_t *out, size_t n) override {
        size_t done = 0;
        while (done < n) {
            size_t h     = head_.load(std::memory_order_acquire);
            size_t t     = tail_.load(std::memory_order_acquire);
            size_t avail = (t - h + BUF_SIZE) % BUF_SIZE;
            if (!avail) break;
            // Contiguous bytes up to ring-buffer wrap-around point
            size_t contig = BUF_SIZE - h;
            size_t take   = avail < (n - done) ? avail : (n - done);
            if (take > contig) take = contig;
            memcpy(out + done, buf_ + h, take);
            head_.store((h + take) % BUF_SIZE, std::memory_order_release);
            done += take;
        }
        return done;
    }

    int read() override {
        uint8_t b;
        return readBytes(&b, 1) == 1 ? (int)b : -1;
    }

    int peek() override {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h != t) ? (int)buf_[h] : -1;
    }

    size_t write(uint8_t) override { return 0; }

    // Stays "valid" until SD is drained AND the ring buffer is empty.
    operator bool() override {
        return available() > 0 || !eof_.load(std::memory_order_acquire);
    }

private:
    File                 *p_file_  = nullptr;
    uint8_t              *buf_     = nullptr;  // PSRAM ring buffer
    uint8_t              *tmp_buf_ = nullptr;  // PSRAM SD read temp (after buf_)
    std::atomic<size_t>   head_{0};  // consumer read index  (Core 1)
    std::atomic<size_t>   tail_{0};  // producer write index (Core 0)
    std::atomic<bool>     eof_{false};
    std::atomic<bool>     stop_{false};
    TaskHandle_t          task_ = nullptr;

    void terminate() {
        stop_.store(true, std::memory_order_release);
        // Wait up to 100 ms for the fill task to self-delete.
        for (int i = 0; i < 100 && task_ != nullptr; i++)
            vTaskDelay(pdMS_TO_TICKS(1));
        if (task_ != nullptr) { vTaskDelete(task_); task_ = nullptr; }
    }

    static void fill_task_fn(void *arg) {
        auto *self = static_cast<PSRAMTaskStream *>(arg);

        while (!self->stop_.load(std::memory_order_acquire)) {
            if (!self->p_file_) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

            // Free space in ring buffer (keep 1 slot to tell full from empty)
            size_t h     = self->head_.load(std::memory_order_acquire);
            size_t t     = self->tail_.load(std::memory_order_relaxed);
            size_t space = (h - t - 1 + BUF_SIZE) % BUF_SIZE;

            if (space < FILL_CHUNK) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }

            int src_avail = self->p_file_->available();
            if (src_avail <= 0) {
                self->eof_.store(true, std::memory_order_release);
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            self->eof_.store(false, std::memory_order_release);

            size_t to_read = (size_t)src_avail < space  ? (size_t)src_avail : space;
            if (to_read > FILL_CHUNK) to_read = FILL_CHUNK;

            size_t got = self->p_file_->read(self->tmp_buf_, to_read);
            if (!got) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }

            // Write into ring buffer, handling wrap-around
            size_t contig = BUF_SIZE - t;
            size_t first  = got < contig ? got : contig;
            memcpy(self->buf_ + t, self->tmp_buf_, first);
            if (got > first)
                memcpy(self->buf_, self->tmp_buf_ + first, got - first);
            self->tail_.store((t + got) % BUF_SIZE, std::memory_order_release);
        }

        self->task_ = nullptr;
        vTaskDelete(nullptr);
    }
};

// BufferedAudioSourceSD: inserts a PSRAMTaskStream between AudioSourceSD
// and the AudioPlayer.  The player reads purely from PSRAM; SD I/O is fully
// decoupled and any SD latency spike (FAT re-allocation, etc.) is absorbed
// without stalling the audio pipeline.
class BufferedAudioSourceSD : public AudioSourceSD {
public:
    BufferedAudioSourceSD(const char *path, const char *ext)
        : AudioSourceSD(path, ext) {}

    Stream *selectStream(int index)        override { return wrap(AudioSourceSD::selectStream(index)); }
    Stream *selectStream(const char *path) override { return wrap(AudioSourceSD::selectStream(path)); }
    Stream *nextStream(int offset)         override { return wrap(AudioSourceSD::nextStream(offset)); }

    // Seek to a byte offset within the open file (called while playing).
    // The WAV header has already been parsed by the decoder; we stay inside
    // the PCM data region (past the 44-byte header).
    bool seekTo(uint32_t byte_offset) { return psramBuf.seekFile(byte_offset); }

private:
    PSRAMTaskStream psramBuf;

    Stream *wrap(Stream *raw) {
        if (!raw) return nullptr;
        psramBuf.end();                 // stop fill task, clear old data
        psramBuf.setFile(&file);        // 'file' is AudioSourceSD::file (protected)
        if (!psramBuf.begin()) return nullptr;
        return &psramBuf;
    }
};

// ─── Audio pipeline objects ───────────────────────────────────────────────────
static I2SStream              i2sOut;
static SonicStream            sonicOut(i2sOut);
static BufferedAudioSourceSD  audioSource("/", ".wav");
static WAVDecoder             wavDecoder;
static AudioPlayer            player(audioSource, sonicOut, wavDecoder);
// ─── Shared playback state (protected by s_state_mutex) ──────────────────────
static SemaphoreHandle_t s_state_mutex = nullptr;
static float    g_speed           = 1.0f;   // 0.2–4.0
static uint8_t  g_volume          = 80;     // 0–100
static bool     g_is_playing      = false;
static bool     g_is_paused       = false;  // true while paused (g_is_playing stays true)
static uint32_t g_song_data_bytes = 0;      // WAV audio data bytes (file size minus 44-byte header)
// Position tracking: audio_pos_s accumulates elapsed audio-seconds at the
// speed that was active at the time.  wall_ref_us is the wall-clock snapshot
// taken when audio_pos_s was last flushed (song start or speed change).
// audio_consumed = g_audio_pos_s + (now - g_wall_ref_us) / 1e6 * g_speed
static float    g_audio_pos_s     = 0.0f;
static int64_t  g_wall_ref_us     = 0;
static AudioInfo g_fmt;

// Commands posted from Core 0 io_task / UART-rx task to Core 1 audio_task
static volatile bool    s_cmd_stop       = false;
static volatile int16_t s_cmd_play_id    = -1;  // ≥0 means "start this song"
static volatile bool    s_cmd_next       = false;
static volatile bool    s_cmd_pause      = false;
static volatile bool    s_cmd_resume     = false;
static volatile int16_t s_cmd_seek_pct   = -1;  // 0–100 seek target, -1 = no pending seek

// Currently highlighted song in the local playlist (tracks encoder position).
// Access only from the io_task (Core 0) – no mutex needed.
static int16_t g_selected_song = 0;  // 0-based index

// ─── Audio helpers ────────────────────────────────────────────────────────────

#define SPEED_MIN  0.4f   // pot fully CCW
#define SPEED_MAX  2.0f   // pot fully CW

// Maps 0–100 to SPEED_MIN–SPEED_MAX linearly (50 = 1.0×).
static float tempo_to_speed(uint8_t t)
{
    return SPEED_MIN + ((float)t / 100.0f) * (SPEED_MAX - SPEED_MIN);
}

static void apply_speed(float speed)
{
    float clamped = speed < SPEED_MIN ? SPEED_MIN : (speed > SPEED_MAX ? SPEED_MAX : speed);
    sonicOut.setSpeed(clamped);
    // Flush audio-time accumulated at the OLD speed before switching.
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    g_audio_pos_s += (float)(now - g_wall_ref_us) / 1000000.0f * g_speed;
    g_wall_ref_us  = now;
    g_speed        = clamped;
    xSemaphoreGive(s_state_mutex);
}

// speed → 0–100 byte (sent over UART); 50 = 1.0×
static uint8_t speed_to_tempo_byte(float speed)
{
    float clamped = speed < SPEED_MIN ? SPEED_MIN : (speed > SPEED_MAX ? SPEED_MAX : speed);
    return (uint8_t)((clamped - SPEED_MIN) / (SPEED_MAX - SPEED_MIN) * 100.0f + 0.5f);
}

// ─── SD playlist scan ────────────────────────────────────────────────────────

static void scan_playlist(void)
{
    File root = SD.open("/");
    if (!root) {
        ESP_LOGE(TAG, "Cannot open SD root");
        return;
    }

    g_song_count = 0;
    ESP_LOGI(TAG, "Scanning SD root for .wav files...");
    while (g_song_count < MAX_SONGS) {
        File f = root.openNextFile();
        if (!f) break;
        if (f.isDirectory()) {
            ESP_LOGD(TAG, "  Skipping dir: %s", f.name());
            f.close();
            continue;
        }

        const char *name = f.name();
        size_t len = strlen(name);
        // Accept only .wav files (case-insensitive suffix check)
        if (len > 4 &&
            (name[len-4] == '.' || name[len-4] == '.') &&
            (name[len-3] == 'w' || name[len-3] == 'W') &&
            (name[len-2] == 'a' || name[len-2] == 'A') &&
            (name[len-1] == 'v' || name[len-1] == 'V'))
        {
            // Store without the .wav extension (display names stay clean;
            // the extension is re-appended when building the playback path).
            size_t copy_len = len - 4;  // drop ".wav"
            if (copy_len >= MAX_NAME) copy_len = MAX_NAME - 1;
            memcpy(g_song_names[g_song_count], name, copy_len);
            g_song_names[g_song_count][copy_len] = '\0';
            ESP_LOGI(TAG, "  [%2u] %s", g_song_count + 1, g_song_names[g_song_count]);
            g_song_count++;
        }
        f.close();
    }
    root.close();

    ESP_LOGI(TAG, "Playlist scan complete: %u WAV file(s) found", g_song_count);
}

// ─── UART callbacks (called from Core 0 UART receive task) ───────────────────

static void on_play_song(uint16_t song_id)
{
    // song_id is 1-based
    if (song_id == 0 || song_id > g_song_count) {
        ESP_LOGW(TAG, "on_play_song: id %u out of range (count=%u)", song_id, g_song_count);
        return;
    }
    ESP_LOGI(TAG, "CMD_PLAY_SONG received: id=%u → '%s'", song_id, g_song_names[song_id - 1]);
    // Post play command for the audio task; the ID is the 0-based index
    s_cmd_play_id = (int16_t)(song_id - 1);
}

static void on_stop_song(void)
{
    ESP_LOGI(TAG, "CMD_STOP_SONG received");
    s_cmd_stop = true;
}

static void on_pause_song(void)
{
    ESP_LOGI(TAG, "CMD_PAUSE received");
    s_cmd_pause = true;
}

static void on_resume_song(void)
{
    ESP_LOGI(TAG, "CMD_RESUME received");
    s_cmd_resume = true;
}

static void on_display_ready(void)
{
    // Display was reset – resend the full song list so it can rebuild its UI.
    ESP_LOGI(TAG, "CMD_DISPLAY_READY received – resending song list (%u tracks)", g_song_count);
    uart_master_send_song_list(
        (const char (*)[UM_MAX_SONG_NAME])g_song_names,
        g_song_count);

    // Re-send current volume and tempo so the display bars are immediately
    // populated after the reset, even before the next potentiometer change.
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uint8_t vol   = g_volume;
    float   speed = g_speed;
    xSemaphoreGive(s_state_mutex);

    uint8_t tempo = speed_to_tempo_byte(speed);
    uart_master_send_poti_update(vol, tempo, 0,
        (uint8_t)(SPEED_MIN * 10.0f + 0.5f),
        (uint8_t)(SPEED_MAX * 10.0f + 0.5f));
    ESP_LOGI(TAG, "CMD_DISPLAY_READY – poti resent: vol=%u tempo=%u", vol, tempo);
}

static void on_seek_song(uint8_t position_pct)
{
    ESP_LOGI(TAG, "CMD_SEEK received: pct=%u", position_pct);
    s_cmd_seek_pct = (int16_t)position_pct;
}

// ─── Format change notifier ───────────────────────────────────────────────────

// Raised by setAudioInfo() when a track starts outside the suppression window.
// This means the AudioPlayer auto-advanced – we want to stop playback.
// Read and cleared only from audio_task (Core 1) – no mutex needed.
static bool    s_track_format_arrived   = false;

// Timestamp (us) of the most recent commanded play (s_cmd_play_id or s_cmd_next).
// setAudioInfo() calls that arrive within CMD_PLAY_SUPPRESS_US of this moment
// are caused by the commanded play itself (player.stop / sonicOut.begin / player.play)
// and must be ignored.  Calls that arrive AFTER the window are auto-advances.
static int64_t s_cmd_play_initiated_us  = 0;
#define CMD_PLAY_SUPPRESS_US  (2000LL * 1000LL)   // 2 seconds

struct FormatChangeNotifier : public AudioInfoSupport {
    void setAudioInfo(AudioInfo info) override {
        g_fmt = info;
        sonicOut.begin(info);
        sonicOut.setSpeed(g_speed);
        if (esp_timer_get_time() - s_cmd_play_initiated_us > CMD_PLAY_SUPPRESS_US) {
            // Outside suppression window → this is an AudioPlayer auto-advance
            s_track_format_arrived = true;
        }
        ESP_LOGI(TAG, "[Format] %d Hz  %dch  %d-bit",
                 info.sample_rate, info.channels, info.bits_per_sample);
    }
    AudioInfo audioInfo() override { return g_fmt; }
} g_formatNotifier;


// ─── Audio task – Core 1 ──────────────────────────────────────────────────────

static void audio_task(void *arg)
{
    ESP_LOGI(TAG, "Audio task running on core %d", xPortGetCoreID());

    while (true) {
        // Handle stop command
        if (s_cmd_stop) {
            s_cmd_stop = false;
            s_cmd_play_initiated_us = esp_timer_get_time();
            player.setMuted(true);  // avoid any trailing audio while stopping
            player.stop();
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            g_is_playing = false;
            g_is_paused  = false;
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "[Audio] Stopped");
        }

        // Handle pause command
        if (s_cmd_pause) {
            s_cmd_pause = false;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (g_is_playing && !g_is_paused) {
                // Flush accumulated audio time before freezing the clock
                int64_t now = esp_timer_get_time();
                g_audio_pos_s += (float)(now - g_wall_ref_us) / 1000000.0f * g_speed;
                g_wall_ref_us  = now;
                g_is_paused    = true;
                ESP_LOGI(TAG, "[Audio] Paused");
            }
            xSemaphoreGive(s_state_mutex);
        }

        // Handle resume command
        if (s_cmd_resume) {
            s_cmd_resume = false;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (g_is_playing && g_is_paused) {
                g_wall_ref_us = esp_timer_get_time(); // restart wall clock from now
                g_is_paused   = false;
                ESP_LOGI(TAG, "[Audio] Resumed");
            }
            xSemaphoreGive(s_state_mutex);
        }

        // Handle seek command – reposition within the currently-playing file.
        int16_t seek_pct = s_cmd_seek_pct;
        if (seek_pct >= 0) {
            s_cmd_seek_pct = -1;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            bool     playing    = g_is_playing;
            uint32_t data_bytes = g_song_data_bytes;
            uint32_t sr         = (uint32_t)g_fmt.sample_rate;
            uint32_t ch         = (uint32_t)g_fmt.channels;
            uint32_t bps        = (uint32_t)(g_fmt.bits_per_sample / 8);
            xSemaphoreGive(s_state_mutex);

            if (playing && data_bytes > 0 && sr > 0) {
                // Compute byte offset, aligned down to a PCM frame boundary.
                uint32_t frame_sz  = ch * bps;
                if (frame_sz == 0) frame_sz = 4; // fallback: stereo 16-bit
                uint32_t raw_data_off = (uint32_t)((uint64_t)seek_pct * data_bytes / 100);
                uint32_t aligned_off  = (raw_data_off / frame_sz) * frame_sz;
                uint32_t file_offset  = 44u + aligned_off;

                ESP_LOGI(TAG, "[Seek] %d%% → file offset %u (aligned)", (int)seek_pct, file_offset);
                audioSource.seekTo(file_offset);

                // Update position tracking to reflect the new playback position.
                uint32_t bytes_per_sec = sr * ch * bps;
                float new_pos_s = (float)aligned_off / (float)bytes_per_sec;
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                g_audio_pos_s = new_pos_s;
                g_wall_ref_us = esp_timer_get_time();
                xSemaphoreGive(s_state_mutex);
            }
        }

        // Handle play-by-ID command
        int16_t play_id = s_cmd_play_id;
        if (play_id >= 0) {
            s_cmd_play_id = -1;
            if (play_id < (int16_t)g_song_count) {
                // Build full path "/filename.wav"
                char path[MAX_NAME + 6];  // '/' + name + '.wav' + NUL
                snprintf(path, sizeof(path), "/%s.wav", g_song_names[play_id]);
                ESP_LOGI(TAG, "[Audio] Playing: %s", path);

                // Mark as playing IMMEDIATELY so io_task disables the encoder
                // Also record the command time so setAudioInfo() calls triggered
                // by stop/flush/play are suppressed (not mistaken for auto-advance).
                s_cmd_play_initiated_us = esp_timer_get_time();
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                g_current_song = play_id;
                g_is_playing   = true;
                g_is_paused    = false;
                xSemaphoreGive(s_state_mutex);

                //player.stop();
                player.setMuted(true);  // avoid any trailing audio while stopping
                player.setPath(path);
                player.play();
                player.setMuted(false);  // avoid any trailing audio while stopping

                // Stat the file to know total audio data bytes (for progress %)
                uint32_t data_bytes = 0;
                File sf = SD.open(path);
                if (sf) {
                    size_t fsz = sf.size();
                    data_bytes = (fsz > 44) ? (uint32_t)(fsz - 44) : 0;
                    sf.close();
                }

                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                g_song_data_bytes = data_bytes;
                g_audio_pos_s     = 0.0f;
                g_wall_ref_us     = esp_timer_get_time();
                xSemaphoreGive(s_state_mutex);
            }
        }

        // Handle next-track command
        if (s_cmd_next) {
            s_cmd_next = false;
            s_cmd_play_initiated_us = esp_timer_get_time(); // suppress format notifications
            player.next();
            int16_t new_song;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            g_current_song = (g_current_song + 1) % (int16_t)g_song_count;
            new_song = g_current_song;
            xSemaphoreGive(s_state_mutex);
            char npath[MAX_NAME + 6];
            snprintf(npath, sizeof(npath), "/%s.wav", g_song_names[new_song]);
            uint32_t ndata = 0;
            File nf = SD.open(npath);
            if (nf) {
                size_t fsz = nf.size();
                ndata = (fsz > 44) ? (uint32_t)(fsz - 44) : 0;
                nf.close();
            }
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            g_song_data_bytes = ndata;
            g_audio_pos_s     = 0.0f;
            g_wall_ref_us     = esp_timer_get_time();
            g_is_playing      = true;
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "[Audio] Next track → [%d] '%s'", new_song, g_song_names[new_song]);
        }

        // Detect AudioPlayer's internal auto-advance (end of track → next song).
        // We do NOT want to continue playing – stop and let the display return
        // to the song list.
        if (s_track_format_arrived) {
            s_track_format_arrived = false;
            // Always an auto-advance here (commanded plays are suppressed in setAudioInfo)
            player.stop();
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            g_is_playing = false;
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "[Audio] Song finished – stopping, returning to song list");
        }

        if (g_is_playing && !g_is_paused) {
            player.copy();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10)); // yield so setup() can finish on Core 1
        }
    }
}

// ─── IO task – Core 0 ────────────────────────────────────────────────────────

static void io_task(void *arg)
{
    ESP_LOGI(TAG, "IO task running on core %d", xPortGetCoreID());

    uint32_t last_status_ms  = 0;
    uint32_t last_poti_ms    = 0;
    uint8_t  last_vol        = 0xFF;
    uint8_t  last_tempo_byte = 0xFF;

    while (true) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        // ── Encoder – ignored while a song is playing or paused ──────────
        // Read and discard PCNT counts regardless so they don't accumulate.
        int16_t steps = encoder_read_steps();
        bool    btn   = encoder_btn_pressed();

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        bool enc_active = g_is_playing;
        xSemaphoreGive(s_state_mutex);

        if (!enc_active) {
            // ── Encoder rotation → update local selection + notify display ──
            if (steps != 0) {
                int8_t clamped = (int8_t)(steps > 127 ? 127 : (steps < -128 ? -128 : steps));

                if (g_song_count > 0) {
                    g_selected_song += clamped;
                    if (g_selected_song < 0) g_selected_song = 0;
                    if (g_selected_song >= (int16_t)g_song_count)
                        g_selected_song = (int16_t)g_song_count - 1;
                }

                ESP_LOGI(TAG, "Encoder move: %+d  →  selected [%d] '%s'",
                         clamped, g_selected_song,
                         g_song_count > 0 ? g_song_names[g_selected_song] : "(none)");
                uart_master_send_encoder_move(clamped);
            }

            // ── Encoder button → notify display; it will send CMD_PLAY_SONG back ──
            if (btn) {
                ESP_LOGI(TAG, "Encoder button: notifying display (selected [%d] '%s')",
                         g_selected_song,
                         g_song_count > 0 ? g_song_names[g_selected_song] : "(none)");
                uart_master_send_encoder_btn();
            }
        }

        // ── Potentiometers every ~10 ms ───────────────────────────────────
        if ((now - last_poti_ms) >= 10) {
            last_poti_ms = now;
            uint8_t vol = 0, tempo_pct = 0;
            bool changed = potis_read(&vol, &tempo_pct);

            if (changed) {
                // Apply volume (0–100 → 0.0–1.0 for AudioTools)
                player.setVolume((float)vol / 100.0f);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                g_volume = vol;
                xSemaphoreGive(s_state_mutex);

                // Apply speed via Sonic
                float speed = tempo_to_speed(tempo_pct);
                apply_speed(speed);

                ESP_LOGI(TAG, "Poti update: vol=%u  tempo=%u%%  speed=%.2fx", vol, tempo_pct, g_speed);

                // Notify display
                uart_master_send_poti_update(vol, tempo_pct, 0,
                    (uint8_t)(SPEED_MIN * 10.0f + 0.5f),
                    (uint8_t)(SPEED_MAX * 10.0f + 0.5f));
                last_vol        = vol;
                last_tempo_byte = tempo_pct;
            }
        }

        // ── Status update every 100 ms → CMD_SET_STATE ───────────────────
        if ((now - last_status_ms) >= 100) {
            last_status_ms = now;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            bool     playing    = g_is_playing;
            bool     paused     = g_is_paused;
            uint8_t  vol        = g_volume;
            float    spd        = g_speed;
            int16_t  cur        = g_current_song;
            uint32_t data_bytes = g_song_data_bytes;
            float    audio_pos  = g_audio_pos_s;
            int64_t  wall_ref   = g_wall_ref_us;
            xSemaphoreGive(s_state_mutex);

            const char *song_name = (cur >= 0 && cur < (int16_t)g_song_count)
                                    ? g_song_names[cur]
                                    : "";
            uint8_t  tempo_byte   = speed_to_tempo_byte(spd);
            uint8_t  position_pct = 0;
            uint16_t duration_s   = 0;

            if (playing && data_bytes > 0 && g_fmt.sample_rate > 0) {
                uint32_t bytes_per_sec = (uint32_t)g_fmt.sample_rate
                                       * (uint32_t)g_fmt.channels
                                       * (uint32_t)(g_fmt.bits_per_sample / 8);
                float total_audio_s = (float)data_bytes / (float)bytes_per_sec;
                // While paused, wall clock is frozen – use only accumulated audio_pos
                float wall_elapsed_s  = paused ? 0.0f
                                               : (float)(esp_timer_get_time() - wall_ref) / 1000000.0f;
                float audio_consumed  = audio_pos + wall_elapsed_s * spd;
                float pct             = audio_consumed / total_audio_s * 100.0f;
                position_pct = (pct >= 100.0f) ? 100u : (pct < 0.0f ? 0u : (uint8_t)pct);
                float dur = total_audio_s / spd;
                duration_s = (dur > 65535.0f) ? 65535u : (uint16_t)dur;
            }

            uart_master_send_state(song_name,
                                   playing ? 1u : 0u,
                                   vol,
                                   tempo_byte,
                                   position_pct,
                                   duration_s);
        }

        vTaskDelay(pdMS_TO_TICKS(5));   // yield 5 ms – enough headroom for UART
    }
}

// ─── I2S test tone ───────────────────────────────────────────────────────────
// Streams a 1 kHz sine wave directly to i2sOut for `duration_ms` milliseconds.
// Call once after i2sOut.begin(), before the AudioPlayer is started.
// Remove the call (or the whole function) once wiring is confirmed.

static void play_test_tone(uint32_t duration_ms)
{
    const int      SAMPLE_RATE  = 44100;
    const int      CHANNELS     = 2;
    const float    FREQ_HZ      = 1000.0f;
    const float    AMPLITUDE    = 0.25f;          // 25 % of full scale
    const int      BUF_SAMPLES  = 256;            // samples per channel per chunk
    const int      BUF_FRAMES   = BUF_SAMPLES * CHANNELS;

    ESP_LOGI(TAG, "[TestTone] Playing %.0f Hz sine for %u ms …", FREQ_HZ, duration_ms);

    const float    step         = 2.0f * (float)M_PI * FREQ_HZ / (float)SAMPLE_RATE;
    float          phase        = 0.0f;
    int16_t        buf[BUF_FRAMES];

    uint32_t       start_ms     = (uint32_t)(esp_timer_get_time() / 1000);

    while ((uint32_t)(esp_timer_get_time() / 1000) - start_ms < duration_ms) {
        for (int i = 0; i < BUF_SAMPLES; i++) {
            int16_t sample = (int16_t)(sinf(phase) * AMPLITUDE * 32767.0f);
            buf[i * 2 + 0] = sample;   // L
            buf[i * 2 + 1] = sample;   // R
            phase += step;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        i2sOut.write((uint8_t *)buf, BUF_FRAMES * sizeof(int16_t));
    }

    ESP_LOGI(TAG, "[TestTone] Done");
}

// ─── Arduino setup ───────────────────────────────────────────────────────────

void setup() 
{

    //disable Bluetooth
    btStop();

    // 2. Power Management configuration: full power
    esp_pm_config_esp32s3_t pm_config = {
        .max_freq_mhz = 240, // Full 240 MHz
        .min_freq_mhz = 240, // Prevent downclocking
        .light_sleep_enable = false // Disable light sleep
    };
    esp_pm_configure(&pm_config);

    Serial.begin(115200);
    AudioLogger::instance().begin(Serial, AudioLogger::Warning);

    ESP_LOGI(TAG, "=== WAV Player Master booting ===");
    ESP_LOGI(TAG, "Pins: SD_CS=%d SCK=%d MOSI=%d MISO=%d", PIN_SD_CS, PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO);
    ESP_LOGI(TAG, "Pins: I2S BCK=%d WS=%d DATA=%d", MY_I2S_BCK, MY_I2S_WS, MY_I2S_DATA);
    ESP_LOGI(TAG, "Pins: UART TX=%d RX=%d  ENC A=%d B=%d BTN=%d", UM_TX_PIN, UM_RX_PIN, ENC_PIN_A, ENC_PIN_B, ENC_PIN_BTN);

    // --- SD card ---
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
    while (!SD.begin(PIN_SD_CS, SPI, 20000000)) {
        ESP_LOGE(TAG, "SD card mount failed – check wiring.");
        delay(1000);
    }
    ESP_LOGI(TAG, "[SD] OK");

    // Scan playlist before setting up UART so we can send CMD_SONG_LIST
    scan_playlist();

    // --- I2S output ---
    I2SConfig i2sCfg  = i2sOut.defaultConfig(TX_MODE);
    i2sCfg.pin_bck         = MY_I2S_BCK;
    i2sCfg.pin_ws          = MY_I2S_WS;
    i2sCfg.pin_data        = MY_I2S_DATA;
    i2sCfg.sample_rate     = 44100;
    i2sCfg.channels        = 2;
    i2sCfg.bits_per_sample = 16;
    i2sCfg.buffer_count    = 64;
    i2sCfg.buffer_size     = 1024;
    i2sCfg.auto_clear    = true;
    i2sOut.begin(i2sCfg);
    ESP_LOGI(TAG, "I2S started: %d Hz  2ch  16-bit  buf=%dx%d",
             i2sCfg.sample_rate, i2sCfg.buffer_count, i2sCfg.buffer_size);

    // ── I2S wiring check: 2-second 1 kHz sine tone ───────────────────────
    // Remove this call once you've confirmed the DAC is wired correctly.
    //play_test_tone(2000);
    // ─────────────────────────────────────────────────────────────────────

    // --- Sonic ---
    g_fmt.sample_rate = 44100; g_fmt.channels = 2; g_fmt.bits_per_sample = 16;
    sonicOut.begin(g_fmt);

    // --- AudioPlayer ---
    player.addNotifyAudioChange(&g_formatNotifier);
    player.setMetadataCallback([](MetaDataType type, const char *str, int len) {
        if (type == MetaDataType::Title)
            ESP_LOGI(TAG, "[Track] %s", str);
    });
    player.begin(false);   // do not auto-play on power-up
    player.setVolume((float)g_volume / 100.0f);
    player.setBufferSize(32768);   // StreamCopy chunk: 32 KB/call – reads from PSRAM, very fast
    ESP_LOGI(TAG, "AudioPlayer started: vol=%u/100  readBuf=65535B", g_volume);

    // --- Shared state mutex ---
    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex != nullptr);

    // --- Hardware inputs ---
    ESP_LOGI(TAG, "Initializing hardware inputs...");
    encoder_init();
    ESP_LOGI(TAG, "Encoder initialized");
    potis_init();
    ESP_LOGI(TAG, "Potentiometers initialized");

    // --- UART master ---
    ESP_LOGI(TAG, "Initializing UART master...");
    uart_master_init(on_play_song, on_stop_song, on_pause_song, on_resume_song, on_display_ready);
    uart_master_set_seek_callback(on_seek_song);
    ESP_LOGI(TAG, "UART master initialized");

    // Give the display a moment to boot, then send the playlist
    ESP_LOGI(TAG, "Waiting 500 ms for display to boot...");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Sending playlist (%u tracks) to display", g_song_count);
    uart_master_send_song_list(
        (const char (*)[UM_MAX_SONG_NAME])g_song_names,
        g_song_count);

    // Attempt initial sync
    bool synced = uart_master_sync(1000);
    ESP_LOGI(TAG, "Initial SYNC: %s", synced ? "ACK received" : "no response (display may not be ready)");

    // --- Start multi-core tasks ---

    // Audio task: Core 1, highest priority to prevent underruns
    xTaskCreatePinnedToCore(
        audio_task, "audio",
        8192, nullptr,
        configMAX_PRIORITIES - 1,
        nullptr,
        1 /* Core 1 */
    );

    // IO task: Core 0, medium priority (UART already has its own task on Core 0)
    xTaskCreatePinnedToCore(
        io_task, "io",
        4096, nullptr,
        configMAX_PRIORITIES - 3,
        nullptr,
        0 /* Core 0 */
    );

    ESP_LOGI(TAG, "=== WAV Player Master ready ===");
}

// ─── Arduino loop (idle – real work is in FreeRTOS tasks) ────────────────────

void loop()
{
    // Intentionally empty – all work runs in audio_task and io_task.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
