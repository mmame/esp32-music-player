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

/* Application modules (pure ESP-IDF, always compiled) */
#include "pins.h"
#include "dimmerlink.h"
#include "encoder.h"
#include "encoder2.h"
#include "potis.h"
#include "uart_master.h"
#include "web_server.h"
#include "song_settings.h"
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
static volatile bool  g_song_loop           = false; /* true: restart on end instead of stop */
static volatile bool  g_song_fixed_speed_en = false; /* true: ignore crank, use fixed speed  */
static volatile float g_song_fixed_speed    = 1.0f;  /* speed multiplier when above is true  */

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
static volatile bool    s_force_poti_resync      = false; /* set by audio_task on unlock */
static volatile bool    s_cmd_wifi_enable        = false; /* set by on_wifi_ctrl(true)  */
static volatile bool    s_cmd_wifi_disable       = false; /* set by on_wifi_ctrl(false) or on_play_song */

static sdmmc_card_t *s_sdcard = nullptr;

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
    int db = (int)(((float)vol / 100.0f) * 64.0f) - 64;
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
    ESP_LOGI(TAG, "Resumed from %.2f s (byte %u)", (double)g_audio_pos_s, file_offset);
}

static void do_seek(uint8_t pct)
{
    if (g_current_song < 0 || pct > 100) return;

    uint32_t frame_sz    = (g_channels * g_bps > 0) ? (uint32_t)(g_channels * g_bps) : 4u;
    uint32_t raw_off     = (uint32_t)(((uint64_t)pct * g_song_bytes) / 100u);
    uint32_t aligned_off = (raw_off / frame_sz) * frame_sz;
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
            } else {
                /* Signal io_task to immediately re-read the live poti value and
                 * forward it to the display without waiting for the knob to move. */
                s_force_poti_resync = true;
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
        s_cmd_play_id    = (int16_t)(song_id - 1);
        s_cmd_wifi_disable = true; /* auto-disable WiFi when playback starts */
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
    } else {
        s_cmd_wifi_disable = true;
        s_cmd_wifi_enable  = false;
    }
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
    if (s.loop)            flags |= 0x01u;
    if (s.fixed_speed > 0.0f) flags |= 0x02u;
    uint8_t spd_x100 = (s.fixed_speed > 0.0f)
                       ? (uint8_t)(s.fixed_speed * 100.0f + 0.5f) : 100u;

    uart_master_send_song_settings(song_id, flags, spd_x100);
}

/**
 * Called when the display sends new settings for a song.
 * Writes (or deletes) the JSON sidecar on the SD card.
 * Runs on the UART rx_task (Core 0).
 */
static void on_set_song_settings(uint16_t song_id,
                                 uint8_t  flags,
                                 uint8_t  fixed_speed_x100)
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
    if (flags == 0) {
        remove(json_path); /* ignore error if file did not exist */
        ESP_LOGI("main", "Removed settings for song %u (all default)", song_id);
        if ((int16_t)(song_id - 1) == g_current_song) {
            g_song_loop           = false;
            g_song_fixed_speed_en = false;
            g_song_fixed_speed    = 1.0f;
        }
        return;
    }

    bool  loop      = (flags & 0x01u) != 0;
    bool  fixed_en  = (flags & 0x02u) != 0;
    float spd       = (fixed_speed_x100 > 0) ? ((float)fixed_speed_x100 / 100.0f) : 1.0f;

    cJSON *root = cJSON_CreateObject();
    if (!root) { ESP_LOGE("main", "OOM creating JSON for song %u", song_id); return; }

    cJSON_AddBoolToObject(root, "loop", loop);
    if (fixed_en) {
        cJSON_AddNumberToObject(root, "fixed_speed", (double)spd);
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
        ESP_LOGI("main", "Applied settings live: loop=%d fixed_en=%d spd=%.2f",
                 (int)loop, (int)fixed_en, fixed_en ? (double)spd : 1.0);
    }
}

/* ======================================================================
 * IO task (Core 0)
 * ====================================================================== */

static void io_task(void *arg)
{
    /* Read initial volume; tempo poti no longer drives playback speed. */
    uint8_t vol = g_volume;
    {
        uint8_t _t = 50;
        potis_read(&vol, &_t);
    }

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
            uint8_t new_vol = vol, _t = 0;
            if (potis_read(&new_vol, &_t)) {
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

        /* ── Organ encoder 2: speed + auto-pause/resume ─────────────────── */
        {
            static bool       s_enc2_was_moving  = false;
            static bool       s_enc2_pause_sent  = false;
            static uint8_t    s_last_tempo_sent  = 255; /* 255 = force first send */
            static TickType_t s_last_tempo_tick  = 0;

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

            /* ── Dimmer: raw crank speed → brightness (no EMA, instant flicker) */
            {
                static uint8_t s_last_dimmer_pct = 255u; /* 255 = force first write */
                float irps = encoder2_get_instant_rps();
                /* Map [0, SPEED_MAX] → [0, 100%]; clamp */
                uint8_t dpct = (irps <= 0.0f) ? 0u
                             : (uint8_t)(irps / SPEED_MAX * 100.0f + 0.5f);
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
                    s_fadein_vol += 1;
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
                    s_fade_vol -= 1;
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
                /* Stop playback and return to song list */
                s_cmd_stop = true;
                uart_master_send_song_list(g_song_names, g_song_count);
            } else {
                /* No song active – forward button to display for navigation */
                uart_master_send_encoder_btn();
            }
        }
        /* btn 1–9: additional buttons, actions to be assigned */

        /* DEBUG: print raw ADC value on BTN_ADC_PIN every 10 s */
        {
            static uint16_t dbg_ctr = 0;
            if (++dbg_ctr >= 1000) {
                dbg_ctr = 0;
                int raw = 0;
                adc_oneshot_read(potis_get_adc_handle(), ADC_CHANNEL_2, &raw);
                ESP_LOGI("BTN_ADC", "raw=%d", raw);
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

            uart_master_send_state(name, (uint8_t)(playing ? 1 : 0),
                                   cur_vol, tempo_byte, pct, dur_s);
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

    /* GPIO ISR service (shared by encoder button and possibly other GPIOs) */
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_ret);
    }

    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex);

    mount_sd();
    scan_playlist();

    web_server_init(player_rescan);

#ifdef HAVE_ADF
    create_pipeline();
#endif

    potis_init();    /* must come before encoder_init() – shares ADC1 handle */
    encoder_init();
    encoder2_init();

    uart_master_init(on_play_song, on_stop_song, on_pause, on_resume, on_display_ready);
    uart_master_set_seek_callback(on_seek);
    uart_master_set_st_bypass_callback(on_st_bypass);
    uart_master_set_tempo_lock_callback(on_tempo_lock);
    uart_master_set_wifi_ctrl_callback(on_wifi_ctrl);
    uart_master_set_song_settings_req_callback(on_song_settings_req);
    uart_master_set_set_song_settings_callback(on_set_song_settings);

    uart_master_send_song_list(g_song_names, g_song_count);
    uart_master_sync(500);

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