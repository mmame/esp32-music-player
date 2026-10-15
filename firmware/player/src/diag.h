/**
 * @file diag.h
 * @brief Diagnostics: SD throughput / latency and CPU load tests, run from the web UI (About tab).
 *
 * Purpose: find out whether the player can afford a SECOND concurrent audio stream (crossfade between
 * two songs), i.e. a second SD read stream plus a second SoundTouch instance, without starving the
 * running playback.  The results are logged to the serial console and streamed to the browser.
 *
 * Best run while a song is playing (realistic contention): start a song, then run the tests.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot of the player state needed by the tests; filled by main.cpp. */
typedef struct {
    int         song_count;
    int         current_idx;     /* -1 = no song loaded */
    bool        playing;         /* pipeline running (not paused / stopped) */
    bool        bypass;          /* SoundTouch bypassed (player mode / fixed 1.0x) */
    float       speed;           /* current tempo */
    float       song_len_s;      /* length of the current song in seconds (media time) */
    float       song_pos_s;      /* position in the current song in seconds (media time) */
    uint32_t    pipe_err_count;  /* pipeline failures detected since boot */
    const char *pipe_err_last;   /* description of the last one ("" if none) */
    uint32_t    sample_rate;     /* of the current song (0 if none) */
    uint8_t     channels;
    uint8_t     bytes_per_sample;
    void       *fatfs, *wav, *sonic, *alc, *lim, *i2s;   /* audio_element_handle_t of the pipeline stages */
    const char *(*song_name)(int idx);   /* song name without ".wav", NULL if out of range */
} diag_ctx_t;

/** Provided by main.cpp. */
void diag_get_context(diag_ctx_t *out);

/** What the SD card negotiated at mount time (shown on the About page). */
typedef struct {
    char     name[8];          /* product name from the card's CID, sanitised */
    char     type[12];         /* "SDSC", "SDHC/SDXC" or "MMC" */
    uint32_t size_mb;
    uint8_t  bus_bits;         /* bus width the host uses */
    uint32_t freq_khz;         /* clock actually in use */
    uint32_t card_limit_khz;   /* highest clock the card accepted (20000 = default speed, 40000 = high speed) */
    uint32_t host_max_khz;     /* what the host asked for */
    char     spec[8];          /* SD physical-layer spec version reported by the card */
} sd_info_t;

/** Provided by main.cpp. Returns false if no card is mounted. */
bool diag_get_sd_info(sd_info_t *out);

/** Provided by main.cpp: the mounted sdmmc_card_t* (NULL if none), for the raw sector test. */
void *diag_get_sd_card(void);

/**
 * Run a diagnostics test and stream the report into @p req (plain text chunks; the caller sends the
 * terminating chunk).
 *
 * @param test      "sd", "cpu", "sdraw" (raw sector reads + clock sweep, playback must be stopped) or "all"
 * @param secs      length of each measurement window in seconds (2..20)
 * @param sim_core  core for the simulated second stream (0 or 1; the SoundTouch element runs on core 1)
 */
esp_err_t diag_run(httpd_req_t *req, const char *test, uint32_t secs, int sim_core);

#ifdef __cplusplus
}
#endif
