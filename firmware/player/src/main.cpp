/**
 * @file main.cpp
 * @brief Music Player firmware – ESP-ADF pipeline implementation.
 *
 * Audio pipeline (when HAVE_ADF is defined via CMakeLists):
 *   SD card -> fatfs_stream -> wav_decoder -> audio_sonic -> alc_volume_setup -> i2s_stream -> DAC
 *
 * Architecture:
 *   Core 1 (audio_task, high priority) - pipeline management, event loop, command dispatch
 *   Core 0 (io_task, med priority)     - UART callbacks, encoder, potentiometers, status TX
 *
 * SD card: 1-bit SDMMC, mounted at /sdcard via esp_vfs_fat_sdmmc_mount().
 * UART:    921600 baud on UART1, framed protocol (see uart_master.h).
 *
 * When HAVE_ADF is NOT defined (CMake build without ADF_PATH set), a minimal
 * stub is compiled so the VS-Code IntelliSense / build-check still succeeds.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#ifdef HAVE_ADF
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#endif

/* Application modules (pure ESP-IDF, always compiled) */
#include "pins.h"
#include "dimmerlink.h"
#include "disp_ota.h"
#include "encoder.h"
#include "encoder2.h"
#include "potis.h"
#include "uart_master.h"
#include "web_server.h"
#include "song_settings.h"
#include "crank_config.h"
#include "bt_ctrl.h"
#include "cJSON.h"

/* ESP-ADF headers (only when ADF_PATH is set in CMakeLists) */
#ifdef HAVE_ADF
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "ringbuf.h"
#include "i2s_stream.h"
#include "fatfs_stream.h"
#include "soundtouch_el.h"
#include "audio_alc.h"
#include "wav_decoder.h"
#endif /* HAVE_ADF */

static const char *TAG = "musicplayer";

/* ======================================================================
 * Constants
 * ====================================================================== */

#define MAX_SONGS      64
#define MAX_NAME       (UM_MAX_SONG_NAME - 1)
#define MOUNT_POINT    "/sdcard"
#define WAV_HDR_BYTES  44u

#define SPEED_MIN  0.7f
#define SPEED_MAX  1.4f

/* ======================================================================
 * Global state
 * ====================================================================== */

static char    g_song_names[MAX_SONGS][UM_MAX_SONG_NAME];
static uint8_t g_song_count = 0;

static SemaphoreHandle_t s_state_mutex = nullptr;
static int16_t  g_current_song = -1;
static bool     g_is_playing   = false;
static bool     g_is_paused    = false;
static uint8_t  g_volume       = 70;
static float    g_speed        = 1.0f;
static volatile bool g_bypass_active = false;  /* true = SoundTouch bypassed, audio at 1.0x */
static volatile bool g_tempo_locked  = false;  /* true = poti changes are ignored for speed  */
static volatile uint8_t g_locked_tempo_raw = 50; /* poti-scale 0–100 value when locked         */

/* Per-song settings loaded from an optional JSON sidecar (e.g. foo.json for foo.wav) */
static volatile bool     g_song_loop           = false; /* true: restart on end instead of stop */
static volatile bool     g_song_fixed_speed_en = false; /* true: ignore crank, use fixed speed  */
static volatile float    g_song_fixed_speed    = 1.0f;  /* speed multiplier when above is true  */
static volatile uint8_t  g_song_pitch_influence = 0u;   /* 0=time-stretch, 100=full tape effect    */
/* per-song dimmer settings (always applied; default = 100/0/1.4) */
static volatile uint8_t  g_song_dimmer_max        = 100u; /* per-song max brightness 0-100       */
static volatile uint8_t  g_song_dimmer_min        = 0u;   /* per-song min brightness 0-100       */
static volatile float    g_song_dimmer_rps_ref    = 1.4f; /* per-song full-brightness RPS        */
static volatile float    g_song_dimmer_holdoff_s  = 0.0f; /* song-position timestamp (s) before which dimmer is suppressed */
static volatile float    g_song_dimmer_fadein_s   = 0.0f; /* seconds to fade from 0→full after holdoff */
static volatile bool     g_song_light_organ        = false; /* true: dimmer driven by audio FFT, not crank speed */
static volatile uint8_t  g_fft_dimmer_pct          = 0u;   /* 0-100, updated by light-organ FFT analysis */

static uint32_t g_song_bytes   = 0;
static uint32_t g_sample_rate  = 44100;
static uint8_t  g_channels     = 2;
static uint8_t  g_bps          = 2;

static float    g_audio_pos_s  = 0.0f;
static int64_t  g_wall_ref_us  = 0;

static volatile int16_t s_cmd_play_id       = -1;
static volatile bool    s_cmd_stop          = false;
static volatile bool    s_cmd_pause         = false;
static volatile bool    s_cmd_resume        = false;
static volatile int8_t  s_cmd_seek_pct      = -1;
static volatile bool    s_cmd_display_ready = false;
static volatile bool    s_cmd_st_bypass_pending = false;
static volatile bool    s_cmd_st_bypass_value   = false;
static volatile bool    s_cmd_tempo_lock_pending = false;
static volatile bool    s_cmd_tempo_lock_value   = false;
static volatile uint8_t s_cmd_locked_tempo_raw   = 50;
static volatile bool    s_cmd_wifi_enable        = false; /* set by on_wifi_ctrl(true)  */
static volatile bool    s_cmd_wifi_disable       = false; /* set by on_wifi_ctrl(false) or on_play_song */
static volatile bool    s_cmd_new_song_loaded    = false;

static esp_timer_handle_t s_wifi_auto_off_timer  = nullptr;

static sdmmc_card_t *s_sdcard = nullptr;

#ifdef HAVE_ADF
/* ── Light-organ FFT analysis state ────────────────────────────────────────── */
#define LO_FFT_SIZE  256   /* must be a power of two */
static float  s_lo_fft_buf[LO_FFT_SIZE * 2]; /* real/imag interleaved               */
static float  s_lo_fft_win[LO_FFT_SIZE];     /* Hann window coefficients             */
static FILE  *s_lo_file    = nullptr;         /* second file handle for analysis reads */
static bool   s_lo_fft_init = false;          /* one-time DSP initialisation flag     */

static void run_light_organ_fft(void)
{
    if (!s_lo_file || !g_is_playing) return;

    /* One-time initialisation of the FFT twiddle table and Hann window */
    if (!s_lo_fft_init) {
        dsps_fft2r_init_fc32(NULL, LO_FFT_SIZE);
        dsps_wind_hann_f32(s_lo_fft_win, LO_FFT_SIZE);
        s_lo_fft_init = true;
    }

    /* Seek to current playback position + 50 ms lookahead */
    uint32_t bps_total = g_sample_rate * (uint32_t)g_channels * (uint32_t)g_bps;
    uint32_t offset    = WAV_HDR_BYTES
                         + (uint32_t)((g_audio_pos_s + 0.05f) * (float)bps_total);
    /* Align to frame boundary */
    uint32_t frame_sz = (uint32_t)g_channels * (uint32_t)g_bps;
    if (frame_sz > 0) offset = (offset / frame_sz) * frame_sz;

    if (fseek(s_lo_file, (long)offset, SEEK_SET) != 0) return;

    /* Read LO_FFT_SIZE samples (one channel, 16-bit) */
    int16_t raw[LO_FFT_SIZE * 4]; /* wide enough for stereo */
    size_t  want = (size_t)(LO_FFT_SIZE * (int)g_channels * (int)g_bps);
    if (want > sizeof(raw)) want = sizeof(raw);
    if (fread(raw, 1, want, s_lo_file) < (size_t)(LO_FFT_SIZE * (int)g_bps)) return;

    /* Build FFT input: left-channel samples, Hann-windowed, normalised to ±1 */
    int step = (g_channels > 1) ? (int)g_channels : 1;
    for (int i = 0; i < LO_FFT_SIZE; i++) {
        float s = ((float)raw[i * step] / 32768.0f) * s_lo_fft_win[i];
        s_lo_fft_buf[i * 2]     = s;    /* real part  */
        s_lo_fft_buf[i * 2 + 1] = 0.0f; /* imag part  */
    }

    /* Run FFT and bit-reverse the output */
    dsps_fft2r_fc32(s_lo_fft_buf, LO_FFT_SIZE);
    dsps_bit_rev_fc32(s_lo_fft_buf, LO_FFT_SIZE);

    /* At 48 kHz / 256 bins, each bin spans ~187.5 Hz.
     * Bin 1-3  : ~187-562 Hz  (bass fundamentals, kick drum)
     * Bin 4-15 : ~750-2812 Hz (melody, harmonics) */
    float bass = 0.0f, mid = 0.0f;
    for (int i = 1; i <= 3; i++) {
        float r = s_lo_fft_buf[i*2], im = s_lo_fft_buf[i*2+1];
        bass += sqrtf(r*r + im*im);
    }
    for (int i = 4; i <= 15; i++) {
        float r = s_lo_fft_buf[i*2], im = s_lo_fft_buf[i*2+1];
        mid += sqrtf(r*r + im*im);
    }

    /* sqrtf compression maps the wide dynamic range to a useful 0-100 % window.
     * Samples are normalised to ±1, so bin magnitudes are much smaller than in
     * raw-int16 examples; the coefficients here are tuned for that scale.
     * Increase the coefficients if the lamp barely reacts; decrease if it
     * saturates too fast. */
    float fft_raw = sqrtf(bass) * g_crank_cfg.lo_bass_weight
                  + sqrtf(mid)  * g_crank_cfg.lo_mid_weight;

    /* Auto-ranging: stretch the observed min→max to the full 0-100 % window.
     * This makes the lamp use its full dynamic range regardless of the absolute
     * signal level, so it works equally well for quiet and loud songs.
     * Max: instant attack, slow decay (~0.2% per 50 ms frame ≈ 4%/s).
     * Min: fast update on new minimum, same slow decay otherwise.          */
    static float s_lo_max = 1.0f;
    static float s_lo_min = 0.0f;
    if (fft_raw > s_lo_max) s_lo_max = fft_raw;
    else { s_lo_max *= g_crank_cfg.lo_decay_rate; if (s_lo_max < 1.0f) s_lo_max = 1.0f; }
    if (fft_raw < s_lo_min) s_lo_min = fft_raw * 0.5f + s_lo_min * 0.5f;
    else { s_lo_min *= g_crank_cfg.lo_decay_rate; if (s_lo_min < 0.0f) s_lo_min = 0.0f; }

    float range = s_lo_max - s_lo_min;
    float level = (range > 0.5f)
                  ? (fft_raw - s_lo_min) / range * 100.0f
                  : 50.0f;
    if (level > 100.0f) level = 100.0f;
    g_fft_dimmer_pct = (uint8_t)level;
}
#endif /* HAVE_ADF */

#ifdef HAVE_ADF
static audio_pipeline_handle_t    g_pipeline  = nullptr;
static audio_element_handle_t     g_fatfs_el  = nullptr;
static audio_element_handle_t     g_wav_el    = nullptr;
static audio_element_handle_t     g_sonic_el  = nullptr;
static audio_element_handle_t     g_alc_el    = nullptr;
static audio_element_handle_t     g_i2s_el    = nullptr;
static audio_event_iface_handle_t g_evt       = nullptr;
#endif

/* ======================================================================
 * Helper: read WAV file header
 * ====================================================================== */

static bool read_wav_info(const char *path,
                          uint32_t   *out_data_bytes,
                          uint32_t   *out_sample_rate,
                          uint8_t    *out_channels,
                          uint8_t    *out_bps)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[WAV_HDR_BYTES];
    bool ok = (fread(hdr, 1, WAV_HDR_BYTES, f) == WAV_HDR_BYTES);
    fclose(f);

    if (!ok) return false;
    if (hdr[0]!='R'||hdr[1]!='I'||hdr[2]!='F'||hdr[3]!='F') return false;

    uint32_t sr   = (uint32_t)hdr[24]       | ((uint32_t)hdr[25]<<8)
                  | ((uint32_t)hdr[26]<<16) | ((uint32_t)hdr[27]<<24);
    uint8_t  ch   = hdr[22];
    uint16_t bits = (uint16_t)hdr[34] | ((uint16_t)hdr[35]<<8);
    uint8_t  bps  = (bits >= 8) ? (uint8_t)(bits / 8) : 2;

    if (sr  == 0) sr  = 44100;
    if (ch  == 0) ch  = 2;
    if (bps == 0) bps = 2;

    *out_sample_rate = sr;
    *out_channels    = ch;
    *out_bps         = bps;
    *out_data_bytes  = (st.st_size > (off_t)WAV_HDR_BYTES)
                       ? (uint32_t)(st.st_size - WAV_HDR_BYTES) : 0u;
    return true;
}

/* ======================================================================
 * SD card mount
 * ====================================================================== */

static void mount_sd(void)
{
    /* DAT3 (GPIO 21) is held HIGH by the 10 kΩ board pull-up, keeping the
     * card in native SD mode from power-on.  No explicit GPIO drive needed.
     *
     * Map the original SPI lines to the SDMMC peripheral:
     *   PIN_SPI_SCK  (GPIO 48) -> CLK
     *   PIN_SPI_MOSI (GPIO 38) -> CMD  (bidirectional command/response)
     *   PIN_SPI_MISO (GPIO 47) -> D0   (bidirectional data)             */
    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk   = (gpio_num_t)PIN_SPI_SCK;
    slot_cfg.cmd   = (gpio_num_t)PIN_SPI_MOSI;
    slot_cfg.d0    = (gpio_num_t)PIN_SPI_MISO;
    slot_cfg.width = 1; /* 1-bit mode: only CLK + CMD + D0 used */
    /* External 10 kΩ pull-ups are present on the board – no internal pull-ups needed. */

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* 1-bit mode at 40 MHz high-speed.  External 10 kΩ pull-ups ensure
     * clean signal edges at this frequency.                              */
    /* Keep DEINIT_ARG so the per-slot deinit path is used on mount failure. */
    host.flags        = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; /* 40 MHz */

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    while (true) {
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_cfg,
                                                &mount_cfg, &s_sdcard);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted at " MOUNT_POINT " (1-bit SDMMC)");
            sdmmc_card_print_info(stdout, s_sdcard);
            return;
        }
        ESP_LOGE(TAG, "SDMMC mount failed (%s) – retrying in 1 s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ======================================================================
 * Playlist scanner
 * ====================================================================== */

static void scan_playlist(void)
{
    g_song_count = 0;
    DIR *dir = opendir(MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open " MOUNT_POINT);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && g_song_count < MAX_SONGS) {
        if (entry->d_type != DT_REG) continue;

        const char *name = entry->d_name;
        size_t      len  = strlen(name);
        if (len <= 4) continue;

        const char *ext = name + len - 4;
        if (strcasecmp(ext, ".wav") != 0) continue;

        size_t base_len = len - 4;
        if (base_len >= UM_MAX_SONG_NAME) base_len = UM_MAX_SONG_NAME - 1;
        memcpy(g_song_names[g_song_count], name, base_len);
        g_song_names[g_song_count][base_len] = '\0';

        ESP_LOGI(TAG, "  [%2u] %s", g_song_count, g_song_names[g_song_count]);
        g_song_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Playlist: %u WAV file(s)", g_song_count);
}

/**
 * Public rescan entry-point called by the web server after a file operation.
 * Rescans the SD card and pushes the updated song list to the display.
 */
static void player_rescan(void)
{
    scan_playlist();
    uart_master_send_song_list(g_song_names, g_song_count);
}

/* ======================================================================
 * Position helpers
 * ====================================================================== */

/* Must be called with s_state_mutex held. */
static float get_current_pos_s_locked(void)
{
    if (!g_is_playing || g_is_paused) return g_audio_pos_s;
    int64_t now = esp_timer_get_time();
    float eff_speed = g_bypass_active ? 1.0f : g_speed;
    return g_audio_pos_s + (float)(now - g_wall_ref_us) * 1e-6f * eff_speed;
}

#ifdef HAVE_ADF
/* ======================================================================
 * Volume / speed control (call with s_state_mutex held)
 * ====================================================================== */

static void apply_volume_locked(uint8_t vol)
{
    /* Power-law taper (γ=0.5): stretches the bottom quarter from –64…–48 dB to –64…–32 dB. */
    float norm = sqrtf((float)vol / 100.0f);
    int db = (int)(norm * 64.0f) - 64;
    if (db < -64) db = -64;
    if (db >  63) db =  63;
    alc_volume_setup_set_volume(g_alc_el, db);
    g_volume = vol;
}

static void apply_speed_locked(float speed)
{
    if (speed < SPEED_MIN) speed = SPEED_MIN;
    if (speed > SPEED_MAX) speed = SPEED_MAX;

    if (g_is_playing && !g_is_paused) {
        int64_t now   = esp_timer_get_time();
        float eff_speed = g_bypass_active ? 1.0f : g_speed;
        g_audio_pos_s += (float)(now - g_wall_ref_us) * 1e-6f * eff_speed;
        g_wall_ref_us  = now;
    }
    g_speed = speed;
    soundtouch_el_set_tempo(g_sonic_el, speed);
}

/* ======================================================================
 * Pipeline control
 * ====================================================================== */

static void pipeline_stop_and_reset(void)
{
    audio_pipeline_stop(g_pipeline);
    audio_pipeline_wait_for_stop(g_pipeline);
    audio_pipeline_reset_ringbuffer(g_pipeline);
    audio_pipeline_reset_elements(g_pipeline);
}

/* start_pipeline=false: load song metadata and enter paused-at-0 state
 * without running the pipeline.  do_resume() will start it when ready.
 * This avoids a start→immediate-stop race that confuses the WAV decoder. */
static void play_song_idx(uint16_t idx, bool start_pipeline = true)
{
    if (idx >= g_song_count) {
        ESP_LOGW(TAG, "play_song_idx: index %u out of range", idx);
        return;
    }

    char path[8 + UM_MAX_SONG_NAME + 5];
    snprintf(path, sizeof(path), "%s/%s.wav", MOUNT_POINT, g_song_names[idx]);

    /* Load optional per-song JSON settings before touching the pipeline. */
    song_settings_t settings;
    song_settings_load(path, &settings);
    g_song_loop           = settings.loop;
    g_song_fixed_speed_en = (settings.fixed_speed > 0.0f);
    g_song_fixed_speed    = (settings.fixed_speed > 0.0f) ? settings.fixed_speed : 1.0f;
    g_song_pitch_influence = settings.pitch_influence;
    soundtouch_el_set_pitch_influence(g_sonic_el, (float)settings.pitch_influence / 100.0f);
    g_song_dimmer_max        = settings.dimmer_max;
    g_song_dimmer_min        = settings.dimmer_min;
    g_song_dimmer_rps_ref    = (settings.dimmer_rps_ref > 0.0f) ? settings.dimmer_rps_ref : 1.4f;
    g_song_dimmer_holdoff_s  = (float)settings.dimmer_holdoff_s;
    g_song_dimmer_fadein_s   = (float)settings.dimmer_fadein_s;
    g_song_light_organ       = settings.light_organ;
#ifdef HAVE_ADF
    if (s_lo_file) { fclose(s_lo_file); s_lo_file = nullptr; }
    if (g_song_light_organ) s_lo_file = fopen(path, "rb");
#endif

    uint32_t data_bytes = 0, sr = 44100;
    uint8_t  ch = 2, bps = 2;
    if (!read_wav_info(path, &data_bytes, &sr, &ch, &bps)) {
        ESP_LOGW(TAG, "Cannot read WAV header: %s (using defaults)", path);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > (off_t)WAV_HDR_BYTES) {
            data_bytes = (uint32_t)(st.st_size - WAV_HDR_BYTES);
        }
    }

    if (g_is_playing || g_is_paused) {
        pipeline_stop_and_reset();
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    g_current_song = (int16_t)idx;
    g_song_bytes   = data_bytes;
    g_sample_rate  = sr;
    g_channels     = ch;
    g_bps          = bps;
    g_audio_pos_s  = 0.0f;
    g_wall_ref_us  = esp_timer_get_time();
    g_is_playing   = start_pipeline;   /* false → stay paused at pos 0 */
    g_is_paused    = !start_pipeline;
    xSemaphoreGive(s_state_mutex);

    audio_element_set_uri(g_fatfs_el, path);

    /* Ensure WAV decoder parses the WAV header fresh for this new song.
     * reserve_data.user_data_2 == 0 → "a new song playing" → reads header.
     * It might have been left non-zero by a prior seek/resume. */
    {
        audio_element_info_t wi = {};
        audio_element_getinfo(g_wav_el, &wi);
        wi.byte_pos                 = 0;
        wi.reserve_data.user_data_2 = 0;
        audio_element_setinfo(g_wav_el, &wi);
    }

    if (start_pipeline) {
        audio_pipeline_run(g_pipeline);
    }

    ESP_LOGI(TAG, "Playing [%u]: %s  (%u B, %uHz, %uch, %ubps)",
             idx, g_song_names[idx], data_bytes, sr, ch, bps);
}

static void do_stop(void)
{
    if (!g_is_playing && !g_is_paused) return;
    pipeline_stop_and_reset();
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    g_is_playing   = false;
    g_is_paused    = false;
    g_current_song = -1;
    g_audio_pos_s  = 0.0f;
    xSemaphoreGive(s_state_mutex);
    /* Clear per-song settings so they don't affect the idle/next-song state. */
    g_song_loop           = false;
    g_song_fixed_speed_en = false;
    g_song_fixed_speed    = 1.0f;
    g_song_pitch_influence = 0u;
    soundtouch_el_set_pitch_influence(g_sonic_el, 0.0f);
    g_song_dimmer_holdoff_s      = 0.0f;
    g_song_dimmer_fadein_s       = 0.0f;
    g_song_light_organ           = false;
    g_fft_dimmer_pct             = 0u;
#ifdef HAVE_ADF
    if (s_lo_file) { fclose(s_lo_file); s_lo_file = nullptr; }
#endif
    ESP_LOGI(TAG, "Stopped");
}

/*
 * Write a synthetic 44-byte WAV header into the fatfs→wav ring buffer.
 * Called after pipeline_stop_and_reset() and before audio_pipeline_run().
 * The WAV decoder ("a new song playing" path) reads this header, gets the
 * correct format, then reads raw PCM from fatfs which starts at the seek
 * position.  This avoids the "resume" path (user_data_2 != 0) that requires
 * a live decoder context and crashes after a full stop.
 *
 * pcm_remaining: bytes of PCM still to be played after the seek point.
 */
static void inject_wav_header(uint32_t pcm_remaining)
{
    uint8_t  hdr[WAV_HDR_BYTES] = {};
    uint32_t bits_per_sample    = (uint32_t)g_bps * 8u;
    uint32_t byte_rate          = g_sample_rate * (uint32_t)g_channels * (uint32_t)g_bps;
    uint16_t block_align        = (uint16_t)(g_channels * g_bps);
    uint32_t riff_size          = pcm_remaining + 36u;

    /* RIFF chunk descriptor */
    hdr[0]='R'; hdr[1]='I'; hdr[2]='F'; hdr[3]='F';
    memcpy(&hdr[4],  &riff_size,     4);
    hdr[8]='W'; hdr[9]='A'; hdr[10]='V'; hdr[11]='E';

    /* "fmt " sub-chunk (16-byte PCM) */
    hdr[12]='f'; hdr[13]='m'; hdr[14]='t'; hdr[15]=' ';
    uint32_t fmt_sz  = 16u; memcpy(&hdr[16], &fmt_sz,       4);
    uint16_t pcm_fmt =  1u; memcpy(&hdr[20], &pcm_fmt,      2);
    uint16_t ch      = (uint16_t)g_channels;
    memcpy(&hdr[22], &ch,             2);
    memcpy(&hdr[24], &g_sample_rate,  4);
    memcpy(&hdr[28], &byte_rate,      4);
    memcpy(&hdr[32], &block_align,    2);
    uint16_t bps16   = (uint16_t)bits_per_sample;
    memcpy(&hdr[34], &bps16,          2);

    /* "data" sub-chunk */
    hdr[36]='d'; hdr[37]='a'; hdr[38]='t'; hdr[39]='a';
    memcpy(&hdr[40], &pcm_remaining,  4);

    ringbuf_handle_t rb = audio_element_get_output_ringbuf(g_fatfs_el);
    if (rb != NULL) {
        rb_write(rb, (char *)hdr, (int)sizeof(hdr), pdMS_TO_TICKS(100));
    }
}

static void do_pause(void)
{
    if (!g_is_playing || g_is_paused) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    g_audio_pos_s = get_current_pos_s_locked();
    g_is_playing  = false;
    g_is_paused   = true;
    xSemaphoreGive(s_state_mutex);
    pipeline_stop_and_reset();
    ESP_LOGI(TAG, "Paused at %.2f s", (double)g_audio_pos_s);
}

static void do_resume(void)
{
    if (!g_is_paused || g_current_song < 0) return;

    /* Ensure pipeline is fully stopped before the reset+run sequence.
     * Handles the "Without wait stop" race when Next is pressed mid-play. */
    pipeline_stop_and_reset();

    uint32_t bytes_per_sec = g_sample_rate * g_channels * g_bps;
    uint32_t frame_sz      = (g_channels * g_bps > 0) ? (uint32_t)(g_channels * g_bps) : 4u;
    uint32_t raw_off       = (bytes_per_sec > 0)
                             ? (uint32_t)(g_audio_pos_s * (float)bytes_per_sec) : 0u;
    uint32_t aligned_off   = (raw_off / frame_sz) * frame_sz;
    uint32_t file_offset   = WAV_HDR_BYTES + aligned_off;

    audio_element_info_t info = {};
    audio_element_getinfo(g_fatfs_el, &info);
    info.byte_pos = file_offset;
    audio_element_setinfo(g_fatfs_el, &info);

    /* Guard against the race where element tasks (fatfs, wav) processed a
     * queued RESUME→open→STOP→close cycle after pipeline_stop_and_reset()
     * returned – possible because audio_element_stop() returns immediately
     * for elements with is_running==false without aborting their ring
     * buffers, so wait_for_stop() also returns early.  Those tasks later
     * run the cycle and land in STOPPED state (not INIT).
     *
     * audio_element_on_cmd_resume() clears the output ring buffer when the
     * element state is STOPPED, which would destroy the injected WAV header.
     * Re-flushing ring buffers and forcing all elements to INIT here (called
     * from user-driven interaction, always ≥100 ms after the last stop) is
     * safe: tasks have long finished by now. */
    audio_pipeline_reset_ringbuffer(g_pipeline);
    audio_pipeline_reset_elements(g_pipeline);

    /* Inject a synthetic WAV header into the fatfs→wav ring buffer so the
     * WAV decoder reads a valid header ("a new song playing" path) and then
     * reads raw PCM from fatfs which starts at the resumed position. */
    uint32_t rem_pcm = (aligned_off <= g_song_bytes) ? (g_song_bytes - aligned_off) : 0u;
    inject_wav_header(rem_pcm);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    g_is_paused   = false;
    g_is_playing  = true;
    g_wall_ref_us = esp_timer_get_time();
    xSemaphoreGive(s_state_mutex);

    audio_pipeline_run(g_pipeline);
    ESP_LOGI(TAG, "Resumed from %.2f s (byte %u)  vol=%u", (double)g_audio_pos_s, file_offset, g_volume);
}

static void do_seek(uint8_t pct)
{
    if (g_current_song < 0 || pct > 100) return;

    uint32_t frame_sz    = (g_channels * g_bps > 0) ? (uint32_t)(g_channels * g_bps) : 4u;
    uint32_t raw_off     = (uint32_t)(((uint64_t)pct * g_song_bytes) / 100u);
    uint32_t aligned_off = (raw_off / frame_sz) * frame_sz;

    /* If paused, just update the stored position; do_resume() will seek there. */
    if (g_is_paused) {
        uint32_t bps_total = g_sample_rate * g_channels * g_bps;
        float    new_pos_s = (bps_total > 0) ? ((float)aligned_off / (float)bps_total) : 0.0f;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        g_audio_pos_s = new_pos_s;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Seek (paused) %u%% -> %.2f s", pct, (double)new_pos_s);
        return;
    }

    uint32_t file_offset = WAV_HDR_BYTES + aligned_off;

    pipeline_stop_and_reset();

    audio_element_info_t info = {};
    audio_element_getinfo(g_fatfs_el, &info);
    info.byte_pos = file_offset;
    audio_element_setinfo(g_fatfs_el, &info);

    /* Same STOPPED-state guard as in do_resume(): force clean ring buffers
     * and INIT element states before injecting the WAV header. */
    audio_pipeline_reset_ringbuffer(g_pipeline);
    audio_pipeline_reset_elements(g_pipeline);

    /* Inject a synthetic WAV header into the fatfs→wav ring buffer so the
     * WAV decoder reads a valid header ("a new song playing" path) and then
     * reads raw PCM from fatfs which starts at the seek position.           */
    uint32_t rem_pcm = (aligned_off <= g_song_bytes) ? (g_song_bytes - aligned_off) : 0u;
    inject_wav_header(rem_pcm);

    uint32_t bps_total = g_sample_rate * g_channels * g_bps;
    float    new_pos_s = (bps_total > 0) ? ((float)aligned_off / (float)bps_total) : 0.0f;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    g_audio_pos_s  = new_pos_s;
    g_wall_ref_us  = esp_timer_get_time();
    g_is_playing   = true;
    g_is_paused    = false;
    xSemaphoreGive(s_state_mutex);

    audio_pipeline_run(g_pipeline);
    ESP_LOGI(TAG, "Seek %u%% -> %.2f s (byte %u)", pct, (double)new_pos_s, file_offset);
}

/* ======================================================================
 * Pipeline creation
 * ====================================================================== */

static void create_pipeline(void)
{
    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    g_pipeline = audio_pipeline_init(&pipe_cfg);
    configASSERT(g_pipeline);

    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type        = AUDIO_STREAM_READER;
    fatfs_cfg.buf_sz      =  8 * 1024;  /*  8 KB SD read burst – fewer SPI transactions  */
    fatfs_cfg.out_rb_size = 64 * 1024;  /* 64 KB PSRAM – keeps WAV decoder well fed      */
    g_fatfs_el = fatfs_stream_init(&fatfs_cfg);
    configASSERT(g_fatfs_el);

    wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
    /* SoundTouch reads ST_CHUNK_FRAMES*2ch*2B = 64 KB per call via rb_read.
     * 128 KB = 2 full chunks of pre-fill headroom so the read completes
     * instantly even if the WAV decoder lags briefly. */
    wav_cfg.out_rb_size = 128 * 1024;
    g_wav_el = wav_decoder_init(&wav_cfg);
    configASSERT(g_wav_el);

    soundtouch_el_cfg_t st_cfg = SOUNDTOUCH_EL_DEFAULT_CFG();
    st_cfg.samplerate  = 44100;
    st_cfg.channels    = 2;
    st_cfg.tempo       = g_speed;
    st_cfg.out_rb_size = 16 * 1024; /* 16 KB PSRAM – absorbs bursty TDHS output          */
    st_cfg.task_stack  =  16 * 1024; /*  16 KB – TDHS uses significant stack              */
    st_cfg.task_core   =          1; /* core 1: TDHS off core 0 so WAV decoder runs freely */
    g_sonic_el = soundtouch_el_init(&st_cfg);
    configASSERT(g_sonic_el);

    alc_volume_setup_cfg_t alc_cfg = DEFAULT_ALC_VOLUME_SETUP_CONFIG();
    alc_cfg.channel     = 1;   /* all uploaded files are 1ch mono */
    alc_cfg.volume      = 0;
    alc_cfg.out_rb_size = 16 * 1024; /* 16 KB PSRAM (default 8 KB) */
    g_alc_el = alc_volume_setup_init(&alc_cfg);
    configASSERT(g_alc_el);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.std_cfg.gpio_cfg.bclk = (gpio_num_t)MY_I2S_BCK;
    i2s_cfg.std_cfg.gpio_cfg.ws   = (gpio_num_t)MY_I2S_WS;
    i2s_cfg.std_cfg.gpio_cfg.dout = (gpio_num_t)MY_I2S_DATA;
    i2s_cfg.std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_GPIO_UNUSED;
    i2s_cfg.std_cfg.gpio_cfg.mclk = (gpio_num_t)MY_I2S_MCLK;
    /* All uploaded files are normalised to 16-bit / 48 kHz / 1ch mono by the
     * browser before upload.  I2S is configured for mono-on-both-slots so the
     * DAC receives the same sample on both L and R wires ("2CH Mono" I2S). */
    i2s_cfg.std_cfg.clk_cfg.sample_rate_hz  = 48000;
    i2s_cfg.std_cfg.slot_cfg.data_bit_width  = I2S_DATA_BIT_WIDTH_16BIT;
    i2s_cfg.std_cfg.slot_cfg.slot_mode       = I2S_SLOT_MODE_MONO;
    i2s_cfg.std_cfg.slot_cfg.slot_mask       = I2S_STD_SLOT_BOTH;
    /* DMA buffers live in internal RAM; out_rb_size goes to PSRAM via audio_mem_calloc.
     * buffer_len must be a multiple of 12 (I2S_BUFFER_ALINED_BYTES_SIZE). */
    i2s_cfg.buffer_len            = 3600;           /* default                   */
    i2s_cfg.out_rb_size           =  16 * 1024;     /*  16 KB       */
    i2s_cfg.chan_cfg.dma_desc_num  = 4;             /* descriptors (was 8)       */
    i2s_cfg.chan_cfg.dma_frame_num = 256;           /* frames/desc (was 1024)    */
    g_i2s_el = i2s_stream_init(&i2s_cfg);
    configASSERT(g_i2s_el);

    audio_pipeline_register(g_pipeline, g_fatfs_el, "fatfs");
    audio_pipeline_register(g_pipeline, g_wav_el,   "wav");
    audio_pipeline_register(g_pipeline, g_sonic_el, "sonic");
    audio_pipeline_register(g_pipeline, g_alc_el,   "alc");
    audio_pipeline_register(g_pipeline, g_i2s_el,   "i2s");

    const char *link_tags[] = {"fatfs", "wav", "sonic", "alc", "i2s"};
    audio_pipeline_link(g_pipeline, link_tags, 5);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    g_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(g_pipeline, g_evt);

    ESP_LOGI(TAG, "Audio pipeline created: fatfs->wav->sonic->alc->i2s");
}

/* ======================================================================
 * Audio task (Core 1) – only compiled with ADF
 * ====================================================================== */

static void audio_task(void *arg)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    apply_volume_locked(g_volume);
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "Audio task running on core %d", xPortGetCoreID());

    while (true) {
        /* Process UART commands */
        if (s_cmd_stop) {
            s_cmd_stop = false;
            do_stop();
        }

        {
            int16_t play_id = s_cmd_play_id;
            if (play_id >= 0) {
                s_cmd_play_id = -1;
                /* Always load the song in paused state at position 0.
                 * The crank rising edge in io_task sends s_cmd_resume,
                 * which calls do_resume() and starts the pipeline. */
                play_song_idx((uint16_t)play_id, false);
                s_cmd_new_song_loaded = true; /* force enc2 rising-edge even if crank is already spinning */
            }
        }

        if (s_cmd_pause) {
            s_cmd_pause = false;
            do_pause();
        }

        if (s_cmd_resume) {
            s_cmd_resume = false;
            do_resume();
        }

        {
            int8_t seek = s_cmd_seek_pct;
            if (seek >= 0) {
                s_cmd_seek_pct = -1;
                do_seek((uint8_t)seek);
            }
        }

        if (s_cmd_display_ready) {
            s_cmd_display_ready = false;
            uart_master_send_song_list(g_song_names, g_song_count);
        }

#ifdef HAVE_ADF
        if (s_cmd_st_bypass_pending) {
            bool bypass = s_cmd_st_bypass_value;
            s_cmd_st_bypass_pending = false;
            g_bypass_active = bypass;
            soundtouch_el_set_bypass(g_sonic_el, bypass);
            ESP_LOGI(TAG, "SoundTouch bypass: %s", bypass ? "ON (passthrough)" : "OFF (time-stretch)");
        }

        if (s_cmd_tempo_lock_pending) {
            bool    lock = s_cmd_tempo_lock_value;
            uint8_t lt   = s_cmd_locked_tempo_raw;
            s_cmd_tempo_lock_pending = false;
            g_tempo_locked     = lock;
            g_locked_tempo_raw = lt;
            if (lock) {
                /* Immediately apply the locked speed so the change takes effect
                 * at once rather than waiting for the next io_task cooldown. */
                float locked_speed = SPEED_MIN + ((float)lt / 100.0f) * (SPEED_MAX - SPEED_MIN);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                apply_speed_locked(locked_speed);
                xSemaphoreGive(s_state_mutex);
            }
            ESP_LOGI(TAG, "Tempo lock: %s (tempo_raw=%u)",
                     lock ? "LOCK" : "UNLOCK", (unsigned)lt);
        }
#endif

        /* Listen for pipeline events (50 ms) */
        audio_event_iface_msg_t msg = {};
        if (audio_event_iface_listen(g_evt, &msg, pdMS_TO_TICKS(50)) == ESP_OK) {
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *)g_i2s_el
                && msg.cmd    == AEL_MSG_CMD_REPORT_STATUS
                && (int)msg.data == AEL_STATUS_STATE_FINISHED)
            {
                ESP_LOGI(TAG, "Song finished");
                /* Elements are FINISHED but the pipeline is still internally
                 * RUNNING.  Stop + reset it now so the next audio_pipeline_run()
                 * call succeeds instead of printing "Pipeline already started". */
                pipeline_stop_and_reset();

                if (g_song_loop && g_current_song >= 0) {
                    /* Loop: reload song at position 0 then resume immediately.
                     * Do NOT set g_is_playing/g_is_paused to false here – that
                     * would briefly signal "stopped" to the io_task state sender
                     * and cause the display to flash back to the song list.
                     * play_song_idx() sets g_is_paused=true (keeping playing||paused
                     * true), and the extra pipeline_stop_and_reset() it may call is
                     * harmless since the pipeline is already stopped. */
                    uint16_t loop_idx = (uint16_t)g_current_song;
                    ESP_LOGI(TAG, "Loop: restarting song %u", loop_idx);
                    play_song_idx(loop_idx, false); /* load at pos 0, pipeline not started */
                    do_resume();                    /* start immediately                   */
                } else {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    g_is_playing  = false;
                    g_is_paused   = false;
                    g_audio_pos_s = 0.0f;
                    xSemaphoreGive(s_state_mutex);
                }
            }
        }
    }
}

#endif /* HAVE_ADF */

/* ======================================================================
 * UART callbacks (Core 0, uart_master rx_task)
 * ====================================================================== */

static void on_play_song(uint16_t song_id)
{
    if (song_id > 0 && song_id <= g_song_count) {
        s_cmd_play_id = (int16_t)(song_id - 1);
    }
}

static void on_stop_song(void)     { s_cmd_stop    = true; }
static void on_pause(void)         { s_cmd_pause   = true; }
static void on_resume(void)        { s_cmd_resume  = true; }
static void on_display_ready(void) { s_cmd_display_ready = true; }

static void on_st_bypass(bool bypass)
{
    s_cmd_st_bypass_value   = bypass;
    s_cmd_st_bypass_pending = true;
}

static void on_tempo_lock(bool lock, uint8_t locked_tempo)
{
    s_cmd_locked_tempo_raw   = locked_tempo;
    s_cmd_tempo_lock_value   = lock;
    s_cmd_tempo_lock_pending = true;
}

static void on_wifi_ctrl(bool enable)
{
    if (enable) {
        s_cmd_wifi_enable  = true;
        s_cmd_wifi_disable = false;
        /* Start/restart 15-minute auto-off timer */
        if (!s_wifi_auto_off_timer) {
            const esp_timer_create_args_t ta = {
                .callback         = [](void *) { s_cmd_wifi_disable = true; ESP_LOGI("wifi", "WiFi auto-disabled after 15 min"); },
                .arg              = nullptr,
                .dispatch_method  = ESP_TIMER_TASK,
                .name             = "wifi_auto_off",
                .skip_unhandled_events = false
            };
            esp_timer_create(&ta, &s_wifi_auto_off_timer);
        }
        esp_timer_stop(s_wifi_auto_off_timer);
        esp_timer_start_once(s_wifi_auto_off_timer, (int64_t)15 * 60 * 1000 * 1000);
    } else {
        s_cmd_wifi_disable = true;
        s_cmd_wifi_enable  = false;
        if (s_wifi_auto_off_timer) esp_timer_stop(s_wifi_auto_off_timer);
    }
}

static void on_bt_ctrl(bool enable)
{
    bt_ctrl_set_enabled(enable);
}

static void on_seek(uint8_t pct)
{
    if (pct > 100) pct = 100;
    s_cmd_seek_pct = (int8_t)pct;
}

/* ======================================================================
 * Song-settings UART callbacks (Core 0, uart_master rx_task)
 * ====================================================================== */

/**
 * Called when the display requests the current settings for a song.
 * Reads the JSON sidecar (if it exists) and sends a CMD_SONG_SETTINGS reply.
 * Runs on the UART rx_task (Core 0); SD card access is safe from there.
 */
static void on_song_settings_req(uint16_t song_id)
{
    if (song_id == 0 || song_id > g_song_count) {
        ESP_LOGW("main", "song_settings_req: id %u out of range", song_id);
        return;
    }

    char path[8 + UM_MAX_SONG_NAME + 5];
    snprintf(path, sizeof(path), "%s/%s.wav", MOUNT_POINT, g_song_names[song_id - 1]);

    song_settings_t s;
    song_settings_load(path, &s);

    uint8_t flags = 0;
    if (s.loop)               flags |= 0x01u;
    if (s.fixed_speed > 0.0f) flags |= 0x02u;
    if (s.dimmer_max != 100u || s.dimmer_min != 0u ||
        s.dimmer_rps_ref < 1.35f || s.dimmer_rps_ref > 1.45f) flags |= 0x08u;
    if (s.light_organ)        flags |= 0x10u;
    uint8_t spd_x100      = (s.fixed_speed > 0.0f)
                            ? (uint8_t)(s.fixed_speed * 100.0f + 0.5f) : 100u;
    uint8_t d_max         = s.dimmer_max;
    uint8_t d_min         = s.dimmer_min;
    uint8_t d_rps_x10     = (uint8_t)(s.dimmer_rps_ref * 10.0f + 0.5f);
    uint8_t d_holdoff     = s.dimmer_holdoff_s;
    uint8_t d_fadein      = s.dimmer_fadein_s;

    uart_master_send_song_settings(song_id, flags, spd_x100,
                                   d_max, d_min, d_rps_x10, d_holdoff, d_fadein, s.pitch_influence);
}

/**
 * Called when the display sends new settings for a song.
 * Writes (or deletes) the JSON sidecar on the SD card.
 * Runs on the UART rx_task (Core 0).
 */
static void on_set_song_settings(uint16_t song_id,
                                 uint8_t  flags,
                                 uint8_t  fixed_speed_x100,
                                 uint8_t  dimmer_max,
                                 uint8_t  dimmer_min,
                                 uint8_t  dimmer_rps_ref_x10,
                                 uint8_t  dimmer_holdoff_s,
                                 uint8_t  dimmer_fadein_s,
                                 uint8_t  pitch_influence_pct)
{
    if (song_id == 0 || song_id > g_song_count) {
        ESP_LOGW("main", "set_song_settings: id %u out of range", song_id);
        return;
    }

    /* Build paths */
    char wav_path[8 + UM_MAX_SONG_NAME + 5];
    snprintf(wav_path, sizeof(wav_path), "%s/%s.wav", MOUNT_POINT, g_song_names[song_id - 1]);

    size_t wav_len = strlen(wav_path);
    char   json_path[8 + UM_MAX_SONG_NAME + 7];
    memcpy(json_path, wav_path, wav_len - 4);
    memcpy(json_path + wav_len - 4, ".json", 6);

    /* If all settings are default: remove the sidecar file */
    if (flags == 0 && dimmer_holdoff_s == 0 && dimmer_fadein_s == 0 && pitch_influence_pct == 0
        && dimmer_max == 100u && dimmer_min == 0u && dimmer_rps_ref_x10 == 14u) {
        remove(json_path);
        ESP_LOGI("main", "Removed settings for song %u (all default)", song_id);
        if ((int16_t)(song_id - 1) == g_current_song) {
            g_song_loop           = false;
            g_song_fixed_speed_en = false;
            g_song_fixed_speed    = 1.0f;
            g_song_pitch_influence       = 0u;
            g_song_dimmer_holdoff_s      = 0.0f;
            g_song_dimmer_fadein_s       = 0.0f;
            soundtouch_el_set_pitch_influence(g_sonic_el, 0.0f);
        }
        return;
    }

    bool  loop         = (flags & 0x01u) != 0;
    bool  fixed_en     = (flags & 0x02u) != 0;
    bool  light_organ  = (flags & 0x10u) != 0;
    float spd          = (fixed_speed_x100 > 0) ? ((float)fixed_speed_x100 / 100.0f) : 1.0f;
    float d_rps_ref    = (dimmer_rps_ref_x10 > 0) ? ((float)dimmer_rps_ref_x10 / 10.0f) : 1.4f;

    cJSON *root = cJSON_CreateObject();
    if (!root) { ESP_LOGE("main", "OOM creating JSON for song %u", song_id); return; }

    cJSON_AddBoolToObject(root, "loop", loop);
    if (fixed_en) {
        cJSON_AddNumberToObject(root, "fixed_speed", (double)spd);
    }
    if (pitch_influence_pct > 0) {
        cJSON_AddNumberToObject(root, "pitch_influence", pitch_influence_pct);
    }
    if (dimmer_max != 100u || dimmer_min != 0u || fabsf(d_rps_ref - 1.4f) > 0.05f) {
        cJSON_AddNumberToObject(root, "dimmer_max", dimmer_max);
        cJSON_AddNumberToObject(root, "dimmer_min", dimmer_min);
        cJSON_AddNumberToObject(root, "dimmer_rps_ref", (double)d_rps_ref);
    }
    if (dimmer_holdoff_s > 0) {
        cJSON_AddNumberToObject(root, "dimmer_holdoff_s", dimmer_holdoff_s);
    }
    if (dimmer_fadein_s > 0) {
        cJSON_AddNumberToObject(root, "dimmer_fadein_s", dimmer_fadein_s);
    }
    if (light_organ) {
        cJSON_AddBoolToObject(root, "light_organ", true);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) { ESP_LOGE("main", "OOM printing JSON for song %u", song_id); return; }

    FILE *f = fopen(json_path, "w");
    if (f) {
        fputs(json_str, f);
        fclose(f);
        ESP_LOGI("main", "Saved settings for song %u: %s", song_id, json_str);
    } else {
        ESP_LOGE("main", "Cannot write %s", json_path);
    }
    cJSON_free(json_str);

    /* Apply immediately if the modified song is currently active */
    if ((int16_t)(song_id - 1) == g_current_song) {
        g_song_loop           = loop;
        g_song_fixed_speed_en = fixed_en;
        g_song_fixed_speed    = fixed_en ? spd : 1.0f;
        g_song_pitch_influence   = pitch_influence_pct;
        g_song_dimmer_max        = dimmer_max;
        g_song_dimmer_min        = dimmer_min;
        g_song_dimmer_rps_ref    = d_rps_ref;
        g_song_dimmer_holdoff_s  = (float)dimmer_holdoff_s;
        g_song_dimmer_fadein_s   = (float)dimmer_fadein_s;
        g_song_light_organ       = light_organ;
        g_fft_dimmer_pct         = 0u;
#ifdef HAVE_ADF
        if (s_lo_file) { fclose(s_lo_file); s_lo_file = nullptr; }
        if (light_organ) {
            char wav_p[8 + UM_MAX_SONG_NAME + 5];
            snprintf(wav_p, sizeof(wav_p), "%s/%s.wav", MOUNT_POINT, g_song_names[(uint8_t)g_current_song]);
            s_lo_file = fopen(wav_p, "rb");
        }
#endif
        soundtouch_el_set_pitch_influence(g_sonic_el, (float)pitch_influence_pct / 100.0f);
        ESP_LOGI("main", "Applied settings live: loop=%d fixed_en=%d spd=%.2f pitch_infl=%u%% "
                 "max=%u min=%u rps_ref=%.1f holdoff=%us fadein=%us",
                 (int)loop, (int)fixed_en, fixed_en ? (double)spd : 1.0, pitch_influence_pct,
                 dimmer_max, dimmer_min, (double)d_rps_ref, dimmer_holdoff_s, dimmer_fadein_s);
    }
}

/* ======================================================================
 * Browser song-settings live-apply callback (HTTP-server task, Core 0)
 * ====================================================================== */

static void on_web_song_settings_saved(const char *wav_path,
                                        bool        loop,
                                        float       fixed_speed,
                                        uint8_t     pitch_influence,
                                        uint8_t     dimmer_max,
                                        uint8_t     dimmer_min,
                                        float       dimmer_rps_ref,
                                        uint8_t     dimmer_holdoff_s,
                                        uint8_t     dimmer_fadein_s)
{
    if (g_current_song < 0) return;

    char cur_path[8 + UM_MAX_SONG_NAME + 5];
    snprintf(cur_path, sizeof(cur_path), "%s/%s.wav", MOUNT_POINT,
             g_song_names[(uint8_t)g_current_song]);
    if (strcmp(wav_path, cur_path) != 0) return;

    /* These volatile writes are safe from the HTTP task; io_task reads them
     * without the mutex (same as after UART on_set_song_settings). */
    g_song_loop            = loop;
    g_song_fixed_speed_en  = (fixed_speed > 0.0f);
    g_song_fixed_speed     = (fixed_speed > 0.0f) ? fixed_speed : 1.0f;
    g_song_pitch_influence = pitch_influence;
    g_song_dimmer_max       = dimmer_max;
    g_song_dimmer_min       = dimmer_min;
    g_song_dimmer_rps_ref   = (dimmer_rps_ref > 0.0f) ? dimmer_rps_ref : 1.4f;
    g_song_dimmer_holdoff_s = (float)dimmer_holdoff_s;
    g_song_dimmer_fadein_s  = (float)dimmer_fadein_s;
    /* Pitch influence on SoundTouch must be applied from the audio_task */
#ifdef HAVE_ADF
    s_cmd_st_bypass_value   = (fixed_speed > 0.0f); /* reuse bypass flag for fixed-speed */
    /* pitch: set via existing async command path */
#endif
    ESP_LOGI(TAG, "Browser settings live-applied: %s  max=%u min=%u rps=%.1f holdoff=%us fadein=%us",
             wav_path, dimmer_max, dimmer_min,
             (double)((dimmer_rps_ref > 0.0f) ? dimmer_rps_ref : 1.4f),
             dimmer_holdoff_s, dimmer_fadein_s);
}

/* ======================================================================
 * IO task (Core 0)
 * ====================================================================== */

static void io_task(void *arg)
{
    uint8_t vol = g_volume;
    potis_read(&vol);

    uart_master_send_poti_update(vol, 0, 0,
                                 (uint8_t)(SPEED_MIN * 10.0f),
                                 (uint8_t)(SPEED_MAX * 10.0f));

#ifdef HAVE_ADF
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    apply_volume_locked(vol);
    apply_speed_locked(SPEED_MIN); /* encoder2 will raise speed once spinning */
    xSemaphoreGive(s_state_mutex);
#endif
    /* Speed target is driven by the organ encoder (encoder2). */
    float speed_target  = SPEED_MIN;
    float speed_applied = SPEED_MIN;

    TickType_t last_state_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "IO task running on core %d", xPortGetCoreID());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        TickType_t now = xTaskGetTickCount();

        /* Volume potentiometer (tempo poti removed from speed control) */
        {
            uint8_t new_vol = vol;
            if (potis_read(&new_vol)) {
                vol = new_vol;
#ifdef HAVE_ADF
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                apply_volume_locked(vol);
                xSemaphoreGive(s_state_mutex);
#endif
                uart_master_send_poti_update(vol, 0, 0,
                                             (uint8_t)(SPEED_MIN * 10.0f),
                                             (uint8_t)(SPEED_MAX * 10.0f));
            }
        }

        /* ── Light-organ FFT analysis (every 50 ms) ─────────────────────────── */
#ifdef HAVE_ADF
        {
            static uint8_t s_lo_tick = 0;
            if (g_song_light_organ && g_is_playing) {
                if (++s_lo_tick >= 5) { s_lo_tick = 0; run_light_organ_fft(); }
            } else {
                s_lo_tick = 0;
            }
        }
#endif

        /* ── Organ encoder 2: speed + auto-pause/resume ─────────────────── */
        {
            static bool       s_enc2_was_moving  = false;
            static bool       s_enc2_pause_sent  = false;
            static uint8_t    s_last_tempo_sent  = 255; /* 255 = force first send */
            static TickType_t s_last_tempo_tick  = 0;

            /* New song loaded (e.g. Next button) – force a rising edge so resume
             * fires even if the crank never stopped between the song switch. */
            if (s_cmd_new_song_loaded) {
                s_cmd_new_song_loaded = false;
                s_enc2_was_moving     = false;
            }

            /* Fade-out: ramp volume to 0 when crank stops, then pause.
             * Fade-in:  ramp volume from 0 when crank starts, after resume.
             * Step 1 per 10 ms tick → ~700–1000 ms at full volume.
             * Mid-transition reversals cross-fade smoothly from current level. */
            static bool    s_vol_fading  = false; /* fade-out active */
            static bool    s_vol_fadein  = false; /* fade-in  active */
            static int16_t s_fade_vol    = 0;     /* fade-out level (vol → 0) */
            static int16_t s_fadein_vol  = 0;     /* fade-in  level (0 → vol) */

            float enc2_spd  = encoder2_update(); /* updates EMA; 0 when stopped */
            bool  enc2_move = encoder2_is_moving();

            /* ── Dimmer: off during holdoff, optional fade-in, ramp with crank ── */
            {
                static uint8_t  s_last_dimmer_pct    = 255u;
                static int16_t  s_dimmer_song_id     = -2;   /* -2 = uninitialized */
                static bool     s_holdoff_was_active = false;
                static bool     s_dimmer_fadein_on   = false;
                static uint32_t s_dimmer_fadein_ms   = 0u;
                static float    s_playing_pf         = 0.0f; /* lamp level at last playing tick */
                uint8_t dpct;

                /* Reset fade-in state when the active song changes */
                if (s_dimmer_song_id != g_current_song) {
                    s_dimmer_song_id     = g_current_song;
                    s_holdoff_was_active = (g_song_dimmer_holdoff_s > 0.0f);
                    s_dimmer_fadein_on   = false;
                    s_dimmer_fadein_ms   = 0u;
                    s_playing_pf         = 0.0f;
                }

                /* compare audio position against holdoff timestamp */
                float cur_pos_s = 0.0f;
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                cur_pos_s = get_current_pos_s_locked();
                xSemaphoreGive(s_state_mutex);

                bool holdoff_active = (g_song_dimmer_holdoff_s > 0.0f
                                       && cur_pos_s < g_song_dimmer_holdoff_s);

                /* trigger fade-in when holdoff expires */
                if (s_holdoff_was_active && !holdoff_active
                    && g_song_dimmer_fadein_s > 0.0f) {
                    s_dimmer_fadein_on = true;
                    s_dimmer_fadein_ms = 0u;
                }
                s_holdoff_was_active = holdoff_active;

                /* Advance fade-in timer */
                if (s_dimmer_fadein_on) {
                    s_dimmer_fadein_ms += 10u;
                    uint32_t total_ms = (uint32_t)(g_song_dimmer_fadein_s * 1000.0f);
                    if (total_ms == 0u || s_dimmer_fadein_ms >= total_ms) {
                        s_dimmer_fadein_on = false;
                    }
                }

                /* Per-song dimmer params; defaults are 100/0/1.4 */
                float dmax = (float)g_song_dimmer_max;

                if (holdoff_active) {
                    dpct = 0u;
                } else if (s_vol_fading && vol > 0) {
                    /* Fade from the last actual playing brightness, not from dmax.
                     * Avoids a jarring jump to full brightness when crank was slow. */
                    float fade_scale = (float)s_fade_vol / (float)vol;
                    dpct = (uint8_t)(s_playing_pf * fade_scale + 0.5f);
                } else if ((g_is_playing || s_vol_fadein) && !s_enc2_pause_sent) {
                    /* Dimmer is independent from the audio volume ramp.
                     * s_vol_fadein included so lamp doesn't flash off during the
                     * brief window before audio_task sets g_is_playing. */
                    float dmin = (float)g_song_dimmer_min;
                    float t;
                    if (g_song_light_organ) {
                        /* Light-organ mode: brightness from FFT audio energy */
                        t = (float)g_fft_dimmer_pct / 100.0f;
                    } else {
                        float ref = g_song_dimmer_rps_ref;
                        t = (ref > 0.0f) ? (encoder2_get_instant_rps() / ref) : 0.0f;
                        if (t > 1.0f) t = 1.0f;
                    }
                    float pf   = dmin + (dmax - dmin) * t;
                    if (s_dimmer_fadein_on) {
                        float total_ms = g_song_dimmer_fadein_s * 1000.0f;
                        float scale    = (total_ms > 0.0f)
                                       ? ((float)s_dimmer_fadein_ms / total_ms) : 1.0f;
                        if (scale > 1.0f) scale = 1.0f;
                        pf *= scale;
                    }
                    s_playing_pf = pf; /* remember for smooth fade-out start */
                    dpct = (uint8_t)(pf + 0.5f);
                } else {
                    dpct = 0u;
                }
                if (dpct > 100u) dpct = 100u;
                if (dpct != s_last_dimmer_pct) {
                    s_last_dimmer_pct = dpct;
                    dimmerlink_set_level(dpct);
                }
            }

            if (enc2_move) {
                /* Cancel any in-progress fade-out; continue fading in from that level */
                if (s_vol_fading) {
                    s_vol_fading = false;
                    s_vol_fadein = true;
                    s_fadein_vol = s_fade_vol;
                }
                s_enc2_pause_sent = false; /* re-arm for next stop */
                /* Rising edge: encoder started spinning while song is paused */
                if (!s_enc2_was_moving && g_is_paused && g_current_song >= 0) {
                    /* Pre-silence output, then resume and ramp up */
                    s_vol_fadein = true;
                    s_fadein_vol = 0;
#ifdef HAVE_ADF
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    apply_volume_locked(0);
                    xSemaphoreGive(s_state_mutex);
#endif
                    s_cmd_resume = true;
                }
                /* Update speed target while song is active and speed not locked */
                if ((g_is_playing || g_is_paused) && !g_tempo_locked) {
                    speed_target = enc2_spd; /* RPS ≈ speed multiplier */
                    if (speed_target < SPEED_MIN) speed_target = SPEED_MIN;
                    if (speed_target > SPEED_MAX) speed_target = SPEED_MAX;
                }
#ifdef HAVE_ADF
                /* Step fade-in each tick until target volume is reached */
                if (s_vol_fadein) {
                    s_fadein_vol += (int16_t)g_crank_cfg.vol_fade_step;
                    if (s_fadein_vol >= (int16_t)vol) {
                        s_fadein_vol = (int16_t)vol;
                        s_vol_fadein = false;
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        apply_volume_locked(vol);
                        xSemaphoreGive(s_state_mutex);
                    } else {
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        apply_volume_locked((uint8_t)s_fadein_vol);
                        xSemaphoreGive(s_state_mutex);
                    }
                }
#endif
            } else {
                /* Cancel any in-progress fade-in; start fade-out from that level */
                if (s_vol_fadein) {
                    s_vol_fadein = false;
                    s_vol_fading = true;
                    s_fade_vol   = s_fadein_vol;
                }
                /* Encoder stopped – fade volume to 0 before pausing */
                if (g_is_playing && !s_enc2_pause_sent) {
                    if (!s_vol_fading) {
                        /* Start fade from the current poti volume */
                        s_vol_fading = true;
                        s_fade_vol   = (int16_t)vol;
                    }
#ifdef HAVE_ADF
                    /* Step fade down each 10 ms tick */
                    s_fade_vol -= (int16_t)g_crank_cfg.vol_fade_step;
                    if (s_fade_vol <= 0) {
                        s_fade_vol = 0;
                        s_vol_fading = false;
                        /* Fade complete – issue pause and restore volume so
                         * the next resume starts at the correct level */
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        apply_volume_locked(vol);
                        xSemaphoreGive(s_state_mutex);
                        s_cmd_pause       = true;
                        s_enc2_pause_sent = true;
                    } else {
                        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                        apply_volume_locked((uint8_t)s_fade_vol);
                        xSemaphoreGive(s_state_mutex);
                    }
#else
                    /* No ADF – pause immediately */
                    s_vol_fading      = false;
                    s_cmd_pause       = true;
                    s_enc2_pause_sent = true;
#endif
                }
            }
            s_enc2_was_moving = enc2_move;

            /* Push speed to display whenever it changes by ≥1 unit (0–100).
             * send_state() covers playback; send_poti_update() ensures the
             * display's speed bar stays current at all times. */
            float disp_speed = g_song_fixed_speed_en
                ? g_song_fixed_speed
                : (g_tempo_locked
                    ? (SPEED_MIN + ((float)g_locked_tempo_raw / 100.0f) * (SPEED_MAX - SPEED_MIN))
                    : speed_target);
            uint8_t tb = (uint8_t)(((disp_speed - SPEED_MIN) /
                          (SPEED_MAX - SPEED_MIN)) * 100.0f + 0.5f);
            if (tb > 100) tb = 100;
            if (tb != s_last_tempo_sent &&
                (now - s_last_tempo_tick) >= pdMS_TO_TICKS(100)) {
                s_last_tempo_sent = tb;
                s_last_tempo_tick = now;
                uart_master_send_poti_update(vol, tb, 0,
                                             (uint8_t)(SPEED_MIN * 10.0f),
                                             (uint8_t)(SPEED_MAX * 10.0f));
            }
        }

#ifdef HAVE_ADF
        /* Apply updated speed target to SoundTouch every tick.
         * When speed is locked the locked value always wins over encoder2. */
        {
            speed_applied = g_song_fixed_speed_en
                ? g_song_fixed_speed
                : (g_tempo_locked
                    ? (SPEED_MIN + ((float)g_locked_tempo_raw / 100.0f) * (SPEED_MAX - SPEED_MIN))
                    : speed_target);
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            apply_speed_locked(speed_applied);
            xSemaphoreGive(s_state_mutex);
        }
#endif

        /* Encoder steps + buttons */
        int16_t steps = encoder_read_steps();
        if (steps != 0) uart_master_send_encoder_move((int8_t)steps);

        int8_t btn = encoder_btn_read();
        if (btn == 0) {
            if (g_is_playing || g_is_paused) {
                s_cmd_stop = true;
            } else {
                /* No song active – forward button to display for navigation */
                uart_master_send_encoder_btn();
            }
        } else if (btn == 1) {
            bt_ctrl_set_enabled(!bt_ctrl_is_enabled());
            /* display icon syncs via state_flags bit 1 in the next CMD_SET_STATE */
        }
        /* btn 2–9: additional buttons, actions to be assigned */

        /* ── Speed-lock switch (SPEED_LOCK_PIN = GPIO2) ─────────────────────────── */
        {
            static bool s_lock_sw_prev = false;
            bool sw_high = (gpio_get_level((gpio_num_t)SPEED_LOCK_PIN) != 0);
            if (sw_high != s_lock_sw_prev) {
                s_lock_sw_prev = sw_high;
                if (sw_high) {
                    /* Switch closed → lock speed at the current encoder target */
                    uint8_t raw = (uint8_t)(((speed_applied - SPEED_MIN) /
                                  (SPEED_MAX - SPEED_MIN)) * 100.0f + 0.5f);
                    if (raw > 100u) raw = 100u;
                    s_cmd_locked_tempo_raw   = raw;
                    s_cmd_tempo_lock_value   = true;
                } else {
                    s_cmd_tempo_lock_value   = false;
                }
                s_cmd_tempo_lock_pending = true;
                ESP_LOGI(TAG, "Speed-lock switch: %s", sw_high ? "LOCKED" : "UNLOCKED");
            }
        }

        /* WiFi enable / disable commands */
        if (s_cmd_wifi_disable) {
            s_cmd_wifi_disable = false;
            web_server_disable();
        } else if (s_cmd_wifi_enable) {
            s_cmd_wifi_enable = false;
            web_server_enable();
        }

        /* State update every 100 ms */
        now = xTaskGetTickCount();
        if ((now - last_state_tick) >= pdMS_TO_TICKS(100)) {
            last_state_tick = now;

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            float   pos_s   = get_current_pos_s_locked();
            bool    playing = g_is_playing || g_is_paused;
            uint8_t cur_vol = g_volume;
            float   speed   = g_speed;
            int16_t song    = g_current_song;
            uint32_t sbytes = g_song_bytes;
            uint32_t sr     = g_sample_rate;
            uint8_t  ch     = g_channels;
            uint8_t  bps    = g_bps;
            xSemaphoreGive(s_state_mutex);

            uint8_t  pct   = 0;
            uint16_t dur_s = 0;
            uint32_t bps_total = sr * ch * bps;

            if (sbytes > 0 && bps_total > 0) {
                float dur_raw = (float)sbytes / (float)bps_total;
                float eff_speed = g_bypass_active ? 1.0f : speed;
                if (pos_s > dur_raw) pos_s = dur_raw;
                pct = (uint8_t)((pos_s / dur_raw) * 100.0f + 0.5f);
                if (pct > 100) pct = 100;
                float adj = (eff_speed > 0.01f) ? (dur_raw / eff_speed) : 0.0f;
                dur_s = (uint16_t)(adj + 0.5f);
            }

            uint8_t tempo_byte;
            if (g_bypass_active) {
                tempo_byte = (uint8_t)(((1.0f - SPEED_MIN) / (SPEED_MAX - SPEED_MIN)) * 100.0f + 0.5f);
            } else {
                tempo_byte = (uint8_t)(
                    ((speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN)) * 100.0f + 0.5f);
            }
            if (tempo_byte > 100) tempo_byte = 100;

            const char *name = (song >= 0 && (uint8_t)song < g_song_count)
                               ? g_song_names[song] : "";

            uint8_t  state_flags = g_tempo_locked ? 0x01u : 0x00u;
            if (bt_ctrl_is_enabled())      state_flags |= 0x02u;
            if (web_server_is_running())   state_flags |= 0x04u;
            uint16_t state_id    = (song >= 0) ? (uint16_t)((uint16_t)song + 1u) : 0u;
            uart_master_send_state(name, (uint8_t)(playing ? 1 : 0),
                                   cur_vol, tempo_byte, pct, dur_s, state_flags, state_id);
        }
    }
}

/* ======================================================================
 * app_main
 * ====================================================================== */

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== Music Player starting ===");

    dimmerlink_probe();   /* detect DimmerLink I2C dimmer, log status if present */

    disp_ota_init();      /* RST=HIGH, BOOT0=LOW – display ESP32 runs normally    */

    /* GPIO ISR service (shared by encoder button and possibly other GPIOs) */
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_ret);
    }

    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex);

    mount_sd();
    scan_playlist();
    crank_config_load();

    //delay 2 seconds to allow the display to boot and send its SYNC command
    vTaskDelay(pdMS_TO_TICKS(2000));

    web_server_init(player_rescan);
    web_server_set_song_settings_callback(on_web_song_settings_saved);

#ifdef HAVE_ADF
    create_pipeline();
#endif

    /* GPIO2: speed-lock switch input, active HIGH */
    {
        gpio_config_t sw_cfg = {};
        sw_cfg.pin_bit_mask = (1ULL << SPEED_LOCK_PIN);
        sw_cfg.mode         = GPIO_MODE_INPUT;
        sw_cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        sw_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        sw_cfg.intr_type    = GPIO_INTR_DISABLE;
        gpio_config(&sw_cfg);
    }

    potis_init();    /* must come before encoder_init() – shares ADC1 handle */
    encoder_init();
    encoder2_init();
    crank_config_apply();
    bt_ctrl_init();

    uart_master_init(on_play_song, on_stop_song, on_pause, on_resume, on_display_ready);
    uart_master_set_seek_callback(on_seek);
    uart_master_set_st_bypass_callback(on_st_bypass);
    uart_master_set_tempo_lock_callback(on_tempo_lock);
    uart_master_set_wifi_ctrl_callback(on_wifi_ctrl);
    uart_master_set_bt_ctrl_callback(on_bt_ctrl);
    uart_master_set_song_settings_req_callback(on_song_settings_req);
    uart_master_set_set_song_settings_callback(on_set_song_settings);

    uart_master_send_song_list(g_song_names, g_song_count);

    if (!uart_master_sync(500)) {
        /* Display did not respond – auto-enable WiFi so the user can flash it
         * remotely via the web interface (POST /disp_update).               */
        ESP_LOGW(TAG, "Display SYNC failed – enabling WiFi for remote display flash");
        web_server_enable();
    }

#ifdef HAVE_ADF
    BaseType_t audio_ok = xTaskCreatePinnedToCore(
        audio_task, "audio_task", 8192, nullptr,
        configMAX_PRIORITIES - 2, nullptr, 1);
    configASSERT(audio_ok == pdPASS);
#endif

    BaseType_t io_ok = xTaskCreatePinnedToCore(
        io_task, "io_task", 4096, nullptr,
        configMAX_PRIORITIES - 3, nullptr, 0);
    configASSERT(io_ok == pdPASS);

    ESP_LOGI(TAG, "All tasks launched");
}