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
 *   SD card → AudioPlayer → WAVDecoder → SonicStream (WSOLA) → I2SStream → DAC
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
#include "SonicStream.h"
#include "uart_master.h"
#include "encoder.h"
#include "potis.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <SD.h>
#include <SPI.h>
#include <string.h>
#include <math.h>
#include "pins.h"

static const char *TAG = "main";


// ─── Playlist ────────────────────────────────────────────────────────────────
#define MAX_SONGS  128
#define MAX_NAME   64    // must match UM_MAX_SONG_NAME

static char     g_song_names[MAX_SONGS][MAX_NAME];
static uint8_t  g_song_count   = 0;
static int16_t  g_current_song = -1;  // index into g_song_names, -1 = none

// ─── Audio pipeline objects ───────────────────────────────────────────────────
static I2SStream     i2sOut;
static SonicStream   sonicOut(i2sOut);
static AudioSourceSD audioSource("/", ".wav");
static WAVDecoder    wavDecoder;
static AudioPlayer   player(audioSource, sonicOut, wavDecoder);

// ─── Shared playback state (protected by s_state_mutex) ──────────────────────
static SemaphoreHandle_t s_state_mutex = nullptr;
static float    g_speed           = 1.0f;   // 0.2–4.0
static uint8_t  g_volume          = 80;     // 0–100
static bool     g_is_playing      = false;
static uint32_t g_song_data_bytes = 0;      // WAV audio data bytes (file size minus 44-byte header)
static int64_t  g_play_start_us   = 0;      // esp_timer_get_time() at song start
static AudioInfo g_fmt;

// Commands posted from Core 0 io_task to Core 1 audio_task
static volatile bool    s_cmd_stop       = false;
static volatile int16_t s_cmd_play_id    = -1; // ≥0 means "start this song"
static volatile bool    s_cmd_next       = false;

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
    g_speed = speed < SPEED_MIN ? SPEED_MIN : (speed > SPEED_MAX ? SPEED_MAX : speed);
    sonicOut.setSpeed(g_speed);
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

// ─── Format change notifier ───────────────────────────────────────────────────

// Set by FormatChangeNotifier::setAudioInfo() whenever a new track's audio
// format arrives (i.e. a new file started decoding).  Read and cleared only
// from audio_task (Core 1) so no mutex is needed.
static bool s_track_format_arrived = false;
// True when the upcoming format change was initiated by an explicit command
// (s_cmd_play_id / s_cmd_next) rather than AudioPlayer's internal auto-advance.
static bool s_track_cmd_initiated  = false;

struct FormatChangeNotifier : public AudioInfoSupport {
    void setAudioInfo(AudioInfo info) override {
        g_fmt = info;
        sonicOut.begin(info);
        sonicOut.setSpeed(g_speed);
        s_track_format_arrived = true;   // signal audio_task that a new track started
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
            player.stop();
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            g_is_playing = false;
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "[Audio] Stopped");
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
                s_track_cmd_initiated = true;   // suppress auto-advance logic
                player.setPath(path);
                player.play();
                // Stat the file to know total audio data bytes (for progress %)
                uint32_t data_bytes = 0;
                File sf = SD.open(path);
                if (sf) {
                    size_t fsz = sf.size();
                    data_bytes = (fsz > 44) ? (uint32_t)(fsz - 44) : 0;
                    sf.close();
                }
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                g_current_song    = play_id;
                g_is_playing      = true;
                g_song_data_bytes = data_bytes;
                g_play_start_us   = esp_timer_get_time();
                xSemaphoreGive(s_state_mutex);
            }
        }

        // Handle next-track command
        if (s_cmd_next) {
            s_cmd_next = false;
            s_track_cmd_initiated = true;   // suppress auto-advance logic
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
            g_play_start_us   = esp_timer_get_time();
            g_is_playing      = true;
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "[Audio] Next track → [%d] '%s'", new_song, g_song_names[new_song]);
        }

        // Detect AudioPlayer's internal auto-advance (end of track → next song)
        if (s_track_format_arrived) {
            s_track_format_arrived = false;
            if (!s_track_cmd_initiated) {
                // Auto-advance: update song index and timing state
                int16_t new_song;
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                g_current_song = (g_current_song + 1) % (int16_t)g_song_count;
                new_song = g_current_song;
                xSemaphoreGive(s_state_mutex);
                char apath[MAX_NAME + 6];
                snprintf(apath, sizeof(apath), "/%s.wav", g_song_names[new_song]);
                uint32_t adata = 0;
                File af = SD.open(apath);
                if (af) {
                    size_t fsz = af.size();
                    adata = (fsz > 44) ? (uint32_t)(fsz - 44) : 0;
                    af.close();
                }
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                g_song_data_bytes = adata;
                g_play_start_us   = esp_timer_get_time();
                g_is_playing      = true;
                xSemaphoreGive(s_state_mutex);
                ESP_LOGI(TAG, "[Audio] Auto-advanced to [%d] '%s'", new_song, g_song_names[new_song]);
            } else {
                s_track_cmd_initiated = false; // consume the flag
            }
        }

        if (g_is_playing) {
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

        // ── Encoder rotation → update local selection + notify display ──
        int16_t steps = encoder_read_steps();
        if (steps != 0) {
            int8_t clamped = (int8_t)(steps > 127 ? 127 : (steps < -128 ? -128 : steps));

            // Advance local selection, clamped to valid playlist range
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

        // ── Encoder button → play selected song ───────────────────────────
        if (encoder_btn_pressed()) {
            int16_t idx = (g_song_count > 0) ? g_selected_song : -1;
            if (idx >= 0) {
                ESP_LOGI(TAG, "Encoder button: starting [%d] '%s'",
                         idx, g_song_names[idx]);
                s_cmd_play_id = idx;
            } else {
                ESP_LOGW(TAG, "Encoder button: no songs in playlist");
            }
            uart_master_send_encoder_btn();
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
                uart_master_send_poti_update(vol, tempo_pct, 0);
                last_vol        = vol;
                last_tempo_byte = tempo_pct;
            }
        }

        // ── Status update every 100 ms → CMD_SET_STATE ───────────────────
        if ((now - last_status_ms) >= 100) {
            last_status_ms = now;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            bool     playing    = g_is_playing;
            uint8_t  vol        = g_volume;
            float    spd        = g_speed;
            int16_t  cur        = g_current_song;
            uint32_t data_bytes = g_song_data_bytes;
            int64_t  start_us   = g_play_start_us;
            xSemaphoreGive(s_state_mutex);

            const char *song_name = (cur >= 0 && cur < (int16_t)g_song_count)
                                    ? g_song_names[cur]
                                    : "";
            uint8_t  tempo_byte   = speed_to_tempo_byte(spd);
            uint8_t  position_pct = 0;
            uint16_t duration_s   = 0;

            // Compute playback position and speed-adjusted duration.
            // g_fmt is written once at song start (Core 1) before io_task reads it;
            // sample_rate is guaranteed non-zero after player.begin().
            if (playing && data_bytes > 0 && g_fmt.sample_rate > 0) {
                uint32_t bytes_per_sec = (uint32_t)g_fmt.sample_rate
                                       * (uint32_t)g_fmt.channels
                                       * (uint32_t)(g_fmt.bits_per_sample / 8);
                float total_audio_s       = (float)data_bytes / (float)bytes_per_sec;
                float speed_adj_dur_s     = total_audio_s / spd;  // shorter when faster
                float elapsed_s           = (float)(esp_timer_get_time() - start_us) / 1000000.0f;
                float pct                 = elapsed_s / speed_adj_dur_s * 100.0f;
                position_pct = (pct >= 100.0f) ? 100u : (pct < 0.0f ? 0u : (uint8_t)pct);
                duration_s   = (speed_adj_dur_s > 65535.0f) ? 65535u : (uint16_t)speed_adj_dur_s;
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
    player.setBufferSize(16384);
    ESP_LOGI(TAG, "AudioPlayer started: vol=%u/100  readBuf=16384 B", g_volume);

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
    uart_master_init(on_play_song, on_stop_song);
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
