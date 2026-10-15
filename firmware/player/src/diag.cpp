/**
 * @file diag.cpp
 * @brief SD throughput / latency and CPU load tests (see diag.h).
 *
 * "sd"  : three phases on the real card, with the player's buffers watched at the same time
 *           S1  one stream, as fast as possible          -> raw throughput / read latency
 *           S2  two streams alternating, as fast as possible -> what two readers get from the bus
 *           S3  two streams paced like real playback    -> bus utilisation and worst-case stall
 * "cpu" : FreeRTOS run-time statistics (per core, per task)
 *           C1  baseline (current playback only)
 *           C2  plus a simulated second stream (second SD read + second SoundTouch, paced like playback)
 */

#include "diag.h"

#ifdef HAVE_ADF

#include "audio_element.h"
#include "audio_mem.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "ringbuf.h"
#include "sdmmc_cmd.h"
#include "soundtouch_el.h"

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "diag";

#define DIAG_MOUNT_POINT  "/sdcard"
#define SD_CHUNK_BYTES    8192          /* the player's fatfs_stream reads 8 KB at a time */
#define OUT_RATE_BPS      96000         /* mono 16-bit 48 kHz: what the I2S side consumes */

static size_t       s_chunk = SD_CHUNK_BYTES;      /* bytes per read in the SD phases */
static uint8_t     *s_dma   = nullptr;             /* internal DMA buffer: when set, read_chunk() uses read() into it */
static bool         s_busy = false;
static httpd_req_t *s_req  = nullptr;

/* ── Output: serial log + browser ─────────────────────────────────────── */

static void out(const char *fmt, ...)
{
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
    if (s_req) {
        size_t len = strlen(buf);
        buf[len]     = '\n';
        buf[len + 1] = '\0';
        httpd_resp_send_chunk(s_req, buf, (ssize_t)(len + 1));
    }
}

/** Internal RAM matters here: DMA-capable reads need internal buffers, and the player's heap is tight. */
static void mem_report(const char *when)
{
    out("  internal RAM %s: %u KB free, largest block %u KB, largest DMA-capable block %u KB",
        when,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024));
}

/* ── Watching the player's own buffers ─────────────────────────────────── */

struct ring_probe {
    const char    *name;
    ringbuf_handle_t rb;
    int            size;       /* bytes */
    int            min_fill;   /* bytes */
    int            rate_bps;   /* consumption rate in bytes/s, to express fill as time */
};

static int        s_play_idx = -1;      /* song that was playing when the test started */
static ring_probe s_rings[4];
static int        s_nrings = 0;

static void rings_init(const diag_ctx_t &c, uint32_t src_rate_bps)
{
    s_nrings = 0;
    if (!c.playing) return;
    struct { const char *name; ringbuf_handle_t rb; int rate; } defs[] = {
        { "SD -> WAV    (fatfs out)",  audio_element_get_output_ringbuf((audio_element_handle_t)c.fatfs), (int)src_rate_bps },
        { "WAV -> ST    (wav out)",    audio_element_get_output_ringbuf((audio_element_handle_t)c.wav),   (int)src_rate_bps },
        { "ST -> volume (sonic out)",  audio_element_get_output_ringbuf((audio_element_handle_t)c.sonic), OUT_RATE_BPS },
        { "-> I2S       (last stage)", audio_element_get_input_ringbuf((audio_element_handle_t)c.i2s),    OUT_RATE_BPS },
    };
    for (auto &d : defs) {
        if (!d.rb) continue;
        s_rings[s_nrings++] = { d.name, d.rb, rb_get_size(d.rb), rb_get_size(d.rb), d.rate };
    }
}

static void rings_reset(void)
{
    for (int i = 0; i < s_nrings; i++) s_rings[i].min_fill = s_rings[i].size;
}

static inline void rings_sample(void)
{
    for (int i = 0; i < s_nrings; i++) {
        int f = rb_bytes_filled(s_rings[i].rb);
        if (f < s_rings[i].min_fill) s_rings[i].min_fill = f;
    }
}

/** Returns the lowest fill level seen, in percent of capacity (100 if not playing). */
static int rings_report(void)
{
    int worst_pct = 100;
    for (int i = 0; i < s_nrings; i++) {
        const ring_probe &r = s_rings[i];
        int pct = r.size > 0 ? (int)((int64_t)r.min_fill * 100 / r.size) : 100;
        if (pct < worst_pct) worst_pct = pct;
        out("    buffer %-26s lowest fill %3d%%  (%d ms left of %d ms)", r.name, pct,
            (int)((int64_t)r.min_fill * 1000 / r.rate_bps), (int)((int64_t)r.size * 1000 / r.rate_bps));
    }
    return worst_pct;
}

/* ── WAV helpers ───────────────────────────────────────────────────────── */

static inline uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool wav_info(FILE *f, int *ch, int *sr, int *bytes_per_sample, long *data_off)
{
    uint8_t h[512];
    fseek(f, 0, SEEK_SET);
    size_t n = fread(h, 1, sizeof(h), f);
    *ch = 2; *sr = 48000; *bytes_per_sample = 2; *data_off = 44;      /* sane defaults */
    if (n < 44 || memcmp(h, "RIFF", 4) != 0) return false;
    size_t pos = 12;
    while (pos + 8 <= n) {
        uint32_t sz = le32(&h[pos + 4]);
        if (memcmp(&h[pos], "fmt ", 4) == 0 && pos + 8 + 16 <= n) {
            *ch = le16(&h[pos + 10]);
            *sr = (int)le32(&h[pos + 12]);
            *bytes_per_sample = le16(&h[pos + 22]) / 8;
        } else if (memcmp(&h[pos], "data", 4) == 0) {
            *data_off = (long)(pos + 8);
            return true;
        }
        pos += 8 + sz + (sz & 1u);
    }
    return false;
}

static bool song_path(const diag_ctx_t &c, int idx, char *dst, size_t n)
{
    const char *name = c.song_name ? c.song_name(idx) : nullptr;
    if (!name) return false;
    snprintf(dst, n, "%s/%s.wav", DIAG_MOUNT_POINT, name);
    return true;
}

/* ── SD tests ──────────────────────────────────────────────────────────── */

struct lat_stats {
    uint64_t bytes;
    uint32_t reads, over50, over100, over200, max_us;
    uint64_t sum_us;
    int      worst_ring_pct;   /* lowest fill of any player buffer during the phase (100 = not playing) */
};

static inline void lat_add(lat_stats &s, size_t n, int64_t us)
{
    s.bytes += n;
    s.reads++;
    s.sum_us += (uint64_t)us;
    if ((uint32_t)us > s.max_us) s.max_us = (uint32_t)us;
    if (us > 50000)  s.over50++;
    if (us > 100000) s.over100++;
    if (us > 200000) s.over200++;
}

/** Read one chunk; wraps to the start of the audio data at EOF. */
static void read_chunk(FILE *f, uint8_t *buf, long data_off, lat_stats &st)
{
    int64_t t0 = esp_timer_get_time();
    size_t got;
    if (s_dma) {
        ssize_t r = read(fileno(f), s_dma, s_chunk);          /* same path as the player's fatfs_dma_stream */
        got = r > 0 ? (size_t)r : 0;
    } else {
        got = fread(buf, 1, s_chunk, f);
    }
    int64_t dt = esp_timer_get_time() - t0;
    if (got < s_chunk) { if (s_dma) lseek(fileno(f), data_off, SEEK_SET); else fseek(f, data_off, SEEK_SET); }
    lat_add(st, got, dt);
}

/** Single-stream max speed through POSIX read() (what ADF's fatfs_stream uses), from a sector-aligned file offset. */
static void read_api_phase(const char *label, const char *path, uint8_t *buf, size_t chunk, uint32_t secs)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { out("    %s: cannot open", label); return; }
    uint64_t bytes = 0, sum_us = 0;
    uint32_t reads = 0, max_us = 0;
    const int64_t t_start = esp_timer_get_time();
    const int64_t t_end   = t_start + (int64_t)secs * 1000000LL;
    while (esp_timer_get_time() < t_end) {
        int64_t t0 = esp_timer_get_time();
        ssize_t got = read(fd, buf, chunk);
        uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        if (got < (ssize_t)chunk) { lseek(fd, 0, SEEK_SET); if (got <= 0) continue; }
        bytes += (uint64_t)got; sum_us += dt; reads++;
        if (dt > max_us) max_us = dt;
    }
    const double elapsed = (double)(esp_timer_get_time() - t_start) / 1e6;
    out("    %-52s %.2f MB/s  (%lu reads of %u KB, avg %.1f ms, max %.1f ms)", label,
        (double)bytes / 1e6 / elapsed, (unsigned long)reads, (unsigned)(chunk / 1024),
        reads ? (double)sum_us / reads / 1000.0 : 0.0, (double)max_us / 1000.0);
    close(fd);
}

/** mode 0: one stream max speed, 1: two streams max speed, 2: two streams paced at the playback rate. */
static lat_stats sd_phase(const char *label, int mode, uint32_t secs, FILE *fa, FILE *fb,
                          uint8_t *buf, long off_a, long off_b, uint32_t rate_bps,
                          int paced_streams = 2, bool show_rings = true)
{
    lat_stats st = {};
    st.worst_ring_pct = 100;
    rings_reset();
    const int64_t t_start = esp_timer_get_time();
    const int64_t t_end   = t_start + (int64_t)secs * 1000000LL;
    const int64_t period  = (int64_t)s_chunk * 1000000LL / (int64_t)rate_bps;
    int64_t next_due = t_start;

    while (esp_timer_get_time() < t_end) {
        if (mode == 2) {
            int64_t wait = next_due - esp_timer_get_time();
            if (wait >= 10000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
            else if (wait > 0) vTaskDelay(1);
            next_due += period;
        }
        read_chunk(fa, buf, off_a, st);
        if (mode == 1 || (mode == 2 && paced_streams >= 2)) read_chunk(fb, buf, off_b, st);
        rings_sample();
    }
    const double elapsed = (double)(esp_timer_get_time() - t_start) / 1e6;
    out("  %s", label);
    out("    %.2f MB/s total  |  %lu reads of %u KB, avg %.1f ms, max %.1f ms  |  bus busy %.0f%%",
        (double)st.bytes / 1e6 / elapsed, (unsigned long)st.reads, (unsigned)(s_chunk / 1024),
        st.reads ? (double)st.sum_us / st.reads / 1000.0 : 0.0, (double)st.max_us / 1000.0,
        (double)st.sum_us / 1e6 / elapsed * 100.0);
    out("    reads slower than 50 / 100 / 200 ms: %lu / %lu / %lu",
        (unsigned long)st.over50, (unsigned long)st.over100, (unsigned long)st.over200);
    if (s_nrings > 0) {
        diag_ctx_t now = {};
        diag_get_context(&now);
        if (!now.playing || now.current_idx != s_play_idx) {
            out("    (!) playback stopped or changed during this phase (song ended?): the buffer numbers below are NOT valid");
            for (int i = 0; i < s_nrings; i++) s_rings[i].min_fill = s_rings[i].size;
        }
    }
    if (show_rings) st.worst_ring_pct = rings_report();
    else {
        for (int i = 0; i < s_nrings; i++) {
            int pct = s_rings[i].size > 0 ? (int)((int64_t)s_rings[i].min_fill * 100 / s_rings[i].size) : 100;
            if (pct < st.worst_ring_pct) st.worst_ring_pct = pct;
        }
    }
    return st;
}

static const char *el_state_name(int st)
{
    switch (st) {
    case 0: return "none";     case 1: return "init";    case 2: return "initializing";
    case 3: return "running";  case 4: return "paused";  case 5: return "stopped";
    case 6: return "finished"; case 7: return "ERROR";   default: return "?";
    }
}

/** Number of pipeline elements in a failed (ERROR) state; prints their states. */
static int report_pipeline(const diag_ctx_t &c, const char *when)
{
    void *const els[] = { c.fatfs, c.wav, c.sonic, c.alc, c.lim, c.i2s };
    static const char *const names[] = { "file", "wav", "soundtouch", "volume", "limiter", "i2s" };
    char line[200];
    int n = snprintf(line, sizeof(line), "  pipeline %s:", when);
    int errors = 0;
    for (int i = 0; i < 6 && n < (int)sizeof(line) - 24; i++) {
        const int st = els[i] ? (int)audio_element_get_state((audio_element_handle_t)els[i]) : 0;
        if (st == AEL_STATE_ERROR) errors++;
        n += snprintf(line + n, sizeof(line) - n, " %s=%s", names[i], el_state_name(st));
    }
    out("%s", line);
    if (c.pipe_err_count) out("  pipeline failures since boot: %lu (last: %s)", (unsigned long)c.pipe_err_count, c.pipe_err_last ? c.pipe_err_last : "");
    return errors;
}

static int64_t reader_pos(const diag_ctx_t &c)
{
    audio_element_info_t info = {};
    if (c.fatfs) audio_element_getinfo((audio_element_handle_t)c.fatfs, &info);
    return info.byte_pos;
}

struct obs_t {
    double  kbps_avg;        /* the player's own file reader, KB/s */
    double  kbps_min;        /* slowest one-second window */
    int     worst_pct;       /* lowest fill of any player buffer */
    lat_stats extra;         /* the paced extra stream (if any) */
};

/** Watch the player for `secs` seconds; optionally read one extra stream paced at `rate_bps` meanwhile. */
static obs_t observe(const diag_ctx_t &c, uint32_t secs, bool extra, FILE *fb, long off_b, uint32_t rate_bps)
{
    obs_t r = {};
    r.kbps_min = 1e9;
    rings_reset();
    const int64_t t0 = esp_timer_get_time(), t_end = t0 + (int64_t)secs * 1000000LL;
    const int64_t period = (int64_t)s_chunk * 1000000LL / (int64_t)rate_bps;
    int64_t next_read = t0, next_sec = t0 + 1000000LL;
    int64_t pos0 = reader_pos(c), last = pos0;
    double bytes_extra = 0;
    while (esp_timer_get_time() < t_end) {
        const int64_t now = esp_timer_get_time();
        if (extra && now >= next_read) { read_chunk(fb, nullptr, off_b, r.extra); next_read += period; }
        rings_sample();
        if (now >= next_sec) {
            const int64_t p = reader_pos(c);
            const double kb = p >= last ? (double)(p - last) / 1000.0 : 0.0;
            if (kb < r.kbps_min) r.kbps_min = kb;
            last = p; next_sec += 1000000LL;
        }
        vTaskDelay(1);
    }
    (void)bytes_extra;
    const int64_t p1 = reader_pos(c);
    const double el = (double)(esp_timer_get_time() - t0) / 1e6;
    r.kbps_avg = p1 >= pos0 ? (double)(p1 - pos0) / 1000.0 / el : 0.0;
    if (r.kbps_min > 1e8) r.kbps_min = 0;
    r.worst_pct = 100;
    for (int i = 1; i < s_nrings; i++) {        /* ring 0 (SD -> WAV) is filled on demand and normally sits low: not a health signal */
        const int pct = s_rings[i].size > 0 ? (int)((int64_t)s_rings[i].min_fill * 100 / s_rings[i].size) : 100;
        if (pct < r.worst_pct) r.worst_pct = pct;
    }
    return r;
}

static void print_obs(const obs_t &o, bool extra, uint32_t rate_bps)
{
    out("    player reader: %.0f KB/s on average, slowest second %.0f KB/s (the song needs about %lu KB/s)",
        o.kbps_avg, o.kbps_min, (unsigned long)(rate_bps / 1000));
    if (extra) {
        out("    extra stream: %lu reads of %u KB, avg %.1f ms, max %.1f ms, bus busy %.0f%%",
            (unsigned long)o.extra.reads, (unsigned)(s_chunk / 1024),
            o.extra.reads ? (double)o.extra.sum_us / o.extra.reads / 1000.0 : 0.0, (double)o.extra.max_us / 1000.0,
            (double)o.extra.sum_us / 1e6 / 6.0 * 100.0);
    }
    rings_report();
}

static void run_sd_test(const diag_ctx_t &c, uint32_t secs)
{
    out("=== SD card test ===");
    if (c.song_count <= 0) { out("No songs found on the SD card."); return; }

    if (report_pipeline(c, "at start") > 0) {
        out("!! a pipeline element is in an ERROR state (a read failed earlier). Stop and restart the song, then run again.");
        return;
    }

    const int ia = (c.current_idx >= 0) ? c.current_idx : 0;
    const int ib = (c.song_count > 1) ? ((ia + 1) % c.song_count) : ia;
    char pa[160], pb[160];
    song_path(c, ia, pa, sizeof(pa));
    song_path(c, ib, pb, sizeof(pb));
    FILE *fa = fopen(pa, "rb");
    FILE *fb = fopen(pb, "rb");
    uint8_t *buf = (uint8_t *)audio_malloc(65536);     /* PSRAM, like the player's own old read buffer */
    if (!fa || !fb || !buf) {
        out("ERROR: cannot open test files / allocate buffer (%s | %s)", pa, pb);
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        audio_free(buf);
        return;
    }
    int ch, sr, bps; long off_a, off_b;
    wav_info(fa, &ch, &sr, &bps, &off_a);
    wav_info(fb, &ch, &sr, &bps, &off_b);
    fseek(fb, 0, SEEK_END);
    long half = (ftell(fb) / 2) & ~0x3FFL;          /* stream B starts mid-file: a different place on the card */
    long start_b = half > off_b ? half : off_b;
    fseek(fb, start_b, SEEK_SET);

    uint32_t rate_bps = (c.playing && c.sample_rate) ? c.sample_rate * c.channels * c.bytes_per_sample : 192000u;
    const float live_tempo = (c.playing && !c.bypass && c.speed > 0.1f) ? c.speed : 1.0f;
    rate_bps = (uint32_t)((float)rate_bps * live_tempo);          /* input is consumed at tempo x real time */
    out("Stream A: %s", pa);
    out("Stream B: %s (from the middle)%s", pb, ia == ib ? " (same file)" : "");
    out("Playback data rate: %lu KB/s per stream (tempo %.2f). Playback is %s.",
        (unsigned long)(rate_bps / 1000), (double)live_tempo,
        c.playing ? "RUNNING" : "NOT running (start a song for a realistic test)");
    s_play_idx = c.current_idx;
    if (c.playing && c.song_len_s > 0.f) {
        const float left = (c.song_len_s - c.song_pos_s) / (live_tempo > 0.1f ? live_tempo : 1.0f);
        const unsigned need = 8 + 2 * secs + 4;
        out("Current song: %.0f s long, at %.0f s: about %.0f s of playback left (this test needs about %u s)",
            (double)c.song_len_s, (double)c.song_pos_s, (double)left, need);
        if (left < (float)need) out("WARNING: the song will end during the test - start a longer song and run again.");
    }
    mem_report("before the test");
    rings_init(c, rate_bps);

    /* S0: read API and buffer memory.  Short, run at low priority so the player is never starved. */
    out("  S0: read API and buffer memory (single stream, short)");
    {
        uint8_t *buf_int = (uint8_t *)heap_caps_aligned_alloc(32, 16384, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_chunk = SD_CHUNK_BYTES;
        fseek(fa, 0, SEEK_SET);
        sd_phase("  fread() 8 KB into PSRAM", 0, 2, fa, fb, buf, 0, 0, rate_bps, 2, false);
        read_api_phase("read() 8 KB into PSRAM  (the old reader)", pa, buf, 8192, 2);
        if (buf_int) {
            read_api_phase("read() 8 KB into internal DMA RAM", pa, buf_int, 8192, 2);
            read_api_phase("read() 16 KB into internal DMA RAM (the new reader)", pa, buf_int, 16384, 2);
        } else {
            out("    (could not allocate a 16 KB internal DMA buffer - see the memory line above)");
        }
        heap_caps_free(buf_int);
    }

    /* The extra stream reads the way the new reader does. */
    s_dma = (uint8_t *)heap_caps_aligned_alloc(32, 16384, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk = 16384;
    fseek(fa, off_a, SEEK_SET);

    if (!c.playing) {
        out("  The player is not playing, so there is no live stream to compare with. Start a song and run again.");
    } else {
        out("  P1: the player alone (%lu s) - nothing else touches the card", (unsigned long)secs);
        obs_t p1 = observe(c, secs, false, fb, start_b, rate_bps);
        print_obs(p1, false, rate_bps);
        diag_ctx_t mid = {}; diag_get_context(&mid);
        const int e1 = report_pipeline(mid, "after P1");

        out("  P2: the player plus ONE extra stream paced at the same rate - a crossfade (%lu s)", (unsigned long)secs);
        fseek(fb, start_b, SEEK_SET);
        obs_t p2 = observe(c, secs, true, fb, start_b, rate_bps);
        print_obs(p2, true, rate_bps);
        diag_ctx_t end = {}; diag_get_context(&end);
        const int e2 = report_pipeline(end, "after P2");

        const bool was_streaming = p1.kbps_avg >= 0.5 * (double)(rate_bps / 1000);
        if (e1 || e2) {
            out("  => RESULT: the pipeline FAILED during the test (see above): not usable as a measurement.");
        } else if (!was_streaming) {
            out("  => RESULT: the player was not streaming normally even in P1 (%.0f KB/s read, %lu KB/s needed): paused, crank stopped, or song ended? Run again while it plays.",
                p1.kbps_avg, (unsigned long)(rate_bps / 1000));
        } else {
            const bool rate_ok = p2.kbps_avg >= 0.9 * p1.kbps_avg;
            const bool buf_ok  = p2.worst_pct >= 25 && p2.worst_pct >= p1.worst_pct - 20;
            out("  => RESULT: player read rate %.0f -> %.0f KB/s with the extra stream (%s); lowest fill of the read-ahead / output buffers %d%% -> %d%% (%s)",
                p1.kbps_avg, p2.kbps_avg, rate_ok ? "unchanged" : "REDUCED", p1.worst_pct, p2.worst_pct, buf_ok ? "OK" : "LOWER");
            out("  => overall: %s", (rate_ok && buf_ok) ? "OK - a second stream does not disturb the player" : "WARNING");
        }
    }

    heap_caps_free(s_dma);
    s_dma = nullptr;
    s_chunk = SD_CHUNK_BYTES;
    fclose(fa);
    fclose(fb);
    audio_free(buf);
}

/* ── Raw SD test: sector reads below the filesystem + clock sweep ───────── */

/** One measurement: sequential raw sector reads of `sectors` each for `secs` seconds. */
static double raw_phase(sdmmc_card_t *card, uint8_t *dmabuf, size_t sectors, uint32_t secs,
                        uint32_t start_sector, uint32_t *errors, double *avg_ms)
{
    const uint32_t limit = (uint32_t)card->csd.capacity;
    uint32_t sector = start_sector;
    uint64_t bytes = 0, sum_us = 0;
    uint32_t reads = 0;
    *errors = 0;
    const int64_t t_start = esp_timer_get_time();
    const int64_t t_end   = t_start + (int64_t)secs * 1000000LL;
    while (esp_timer_get_time() < t_end) {
        if (sector + sectors >= limit) sector = start_sector;
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = sdmmc_read_sectors(card, dmabuf, sector, sectors);
        int64_t dt = esp_timer_get_time() - t0;
        if (e != ESP_OK) { (*errors)++; continue; }
        bytes += sectors * card->csd.sector_size;
        sum_us += (uint64_t)dt;
        reads++;
        sector += (uint32_t)sectors;
    }
    const double elapsed = (double)(esp_timer_get_time() - t_start) / 1e6;
    *avg_ms = reads ? (double)sum_us / reads / 1000.0 : 0.0;
    return (double)bytes / 1e6 / elapsed;
}

static void run_sdraw_test(const diag_ctx_t &c, uint32_t secs)
{
    out("=== Raw SD test: sector reads below the filesystem (%lu s per line) ===", (unsigned long)secs);
    sdmmc_card_t *card = (sdmmc_card_t *)diag_get_sd_card();
    if (!card) { out("No SD card mounted."); return; }
    if (c.playing) {
        out("Playback is running: stop it first (this test bypasses the filesystem lock and must not run alongside it).");
        return;
    }
    uint8_t *dmabuf = (uint8_t *)heap_caps_aligned_alloc(32, 32768, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!dmabuf) { out("ERROR: cannot allocate a 32 KB DMA buffer"); return; }

    const int slot = card->host.slot;
    const uint32_t orig_khz = (uint32_t)card->real_freq_khz;
    const uint32_t start = (uint32_t)(card->csd.capacity / 2) & ~0xFFFu;     /* middle of the card, read-only */
    uint32_t errors;
    double avg_ms, mbps;

    out("Card clock in use: %lu kHz.  Theoretical 1-bit limit: clock / 8  (= %.2f MB/s at this clock)",
        (unsigned long)orig_khz, (double)orig_khz / 8000.0);

    out("  R1: raw reads at the current clock, different read sizes");
    static const size_t counts[] = { 1, 8, 32, 64 };
    for (size_t n : counts) {
        mbps = raw_phase(card, dmabuf, n, secs > 3 ? 3 : secs, start, &errors, &avg_ms);
        out("    %2u sectors (%5.1f KB) per read: %.2f MB/s, avg %.2f ms per read, errors %lu",
            (unsigned)n, (double)n * 0.5, mbps, avg_ms, (unsigned long)errors);
    }

    out("  R2: clock sweep with 32 KB raw reads");
    static const uint32_t clocks[] = { 40000, 20000, 10000, 5000 };
    double mb_hi = 0.0, mb_lo = 0.0;
    for (uint32_t khz : clocks) {
        if (khz > orig_khz) continue;
        if (sdmmc_host_set_card_clk(slot, khz) != ESP_OK) { out("    %lu kHz: cannot set", (unsigned long)khz); continue; }
        mbps = raw_phase(card, dmabuf, 64, secs > 3 ? 3 : secs, start, &errors, &avg_ms);
        out("    %5.1f MHz: %.2f MB/s (%.0f%% of the 1-bit limit), errors %lu",
            (double)khz / 1000.0, mbps, mbps / ((double)khz / 8000.0) * 100.0, (unsigned long)errors);
        if (khz == orig_khz) mb_hi = mbps;
        if (khz == 10000)    mb_lo = mbps;
    }
    sdmmc_host_set_card_clk(slot, orig_khz);                  /* restore */

    if (mb_hi > 0.0 && mb_lo > 0.0) {
        const double ratio = mb_hi / mb_lo;
        out("  => 4x the clock gives %.1fx the speed: %s", ratio,
            ratio > 2.5 ? "speed follows the clock (bus-bound): a clock/signal problem could be limiting it"
                        : "speed barely depends on the clock: the card or the driver is the limit, not the bus clock");
    }
    heap_caps_free(dmabuf);
}

/* ── CPU tests ─────────────────────────────────────────────────────────── */

struct snap_t {
    TaskStatus_t *arr;
    UBaseType_t   n;
    uint32_t      total;
};

static bool snap_take(snap_t *s)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    s->arr = (TaskStatus_t *)calloc(cap, sizeof(TaskStatus_t));
    if (!s->arr) { s->n = 0; return false; }
    s->n = uxTaskGetSystemState(s->arr, cap, &s->total);
    return s->n > 0;
}

static void snap_free(snap_t *s) { free(s->arr); s->arr = nullptr; s->n = 0; }

struct cpu_row { char name[20]; int core; float pct; };

/** Prints per-core load and the busiest tasks; returns the highest core load (%) and the % of `watch`. */
static float cpu_report(const snap_t &a, const snap_t &b, const char *watch, float *watch_pct)
{
    const float dtotal = (float)(b.total - a.total);
    float core_load[2] = { 0.f, 0.f };
    cpu_row rows[40];
    int nrows = 0;
    if (watch_pct) *watch_pct = 0.f;
    if (dtotal <= 0.f) { out("    (no run-time data)"); return 0.f; }

    for (UBaseType_t i = 0; i < b.n; i++) {
        const TaskStatus_t &tb = b.arr[i];
        uint32_t before = 0;
        for (UBaseType_t j = 0; j < a.n; j++) {
            if (a.arr[j].xHandle == tb.xHandle) { before = a.arr[j].ulRunTimeCounter; break; }
        }
        const float pct = 100.f * (float)(tb.ulRunTimeCounter - before) / dtotal;
        const int core = (tb.xCoreID == tskNO_AFFINITY) ? -1 : (int)tb.xCoreID;
        if (strncmp(tb.pcTaskName, "IDLE", 4) == 0) {
            if (core == 0 || core == 1) core_load[core] = 100.f - pct;
            continue;
        }
        if (watch && strcmp(tb.pcTaskName, watch) == 0 && watch_pct) *watch_pct = pct;
        if (nrows < 40) {
            strlcpy(rows[nrows].name, tb.pcTaskName, sizeof(rows[nrows].name));
            rows[nrows].core = core;
            rows[nrows].pct  = pct;
            nrows++;
        }
    }
    out("    CPU load: core 0 = %.0f%%   core 1 = %.0f%%", core_load[0], core_load[1]);
    for (int pass = 0; pass < 8; pass++) {                   /* top 8 tasks, simple selection */
        int best = -1;
        for (int i = 0; i < nrows; i++) if (rows[i].pct >= 0.f && (best < 0 || rows[i].pct > rows[best].pct)) best = i;
        if (best < 0 || rows[best].pct < 0.5f) break;
        out("      %-18s core %s  %5.1f%% of one core", rows[best].name,
            rows[best].core == 0 ? "0" : (rows[best].core == 1 ? "1" : "-"), rows[best].pct);
        rows[best].pct = -1.f;
    }
    return core_load[0] > core_load[1] ? core_load[0] : core_load[1];
}

/** Sample the player buffers every 100 ms while waiting. */
static void wait_sampling(uint32_t secs)
{
    const int64_t t_end = esp_timer_get_time() + (int64_t)secs * 1000000LL;
    while (esp_timer_get_time() < t_end) {
        rings_sample();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Simulated second stream: reads a second song and runs a second SoundTouch instance, paced exactly
 * like real playback (input consumed at tempo x real time). */
struct sim_ctx {
    char           path[160];
    float          tempo;
    volatile bool  stop;
    volatile bool  done;
    volatile bool  failed;
    volatile uint64_t frames_in;
    volatile uint32_t max_late_ms;
    int            sr;
};

static void sim_task(void *arg)
{
    sim_ctx *c = (sim_ctx *)arg;
    FILE *f = fopen(c->path, "rb");
    int ch = 2, sr = 48000, bps = 2; long off = 44;
    soundtouch_sim_handle_t st = nullptr;
    const int chunk_frames = 2048;               /* 8 KB per read for stereo: small internal DMA buffer */
    uint8_t *buf = nullptr;
    int16_t *mono = nullptr;

    if (f) {
        wav_info(f, &ch, &sr, &bps, &off);
        if (bps != 2 || ch < 1 || ch > 2) { ch = 2; bps = 2; }
        c->sr = sr;
        st   = soundtouch_sim_create(sr, c->tempo);
        buf  = (uint8_t *)heap_caps_aligned_alloc(32, (size_t)chunk_frames * ch * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        mono = (int16_t *)audio_malloc((size_t)chunk_frames * 2);
    }
    if (!f || !st || !buf || !mono) {
        ESP_LOGE(TAG, "second stream setup failed: file=%p st=%p buf=%p mono=%p", (void *)f, (void *)st, (void *)buf, (void *)mono);
        c->failed = true;
    } else {
        fseek(f, 0, SEEK_END);
        long start = (ftell(f) / 2) & ~0x3FFL;                 /* second stream starts mid-file */
        fseek(f, start > off ? start : off, SEEK_SET);
        const int64_t t0 = esp_timer_get_time();
        while (!c->stop) {
            ssize_t n = read(fileno(f), buf, (size_t)chunk_frames * ch * 2);
            if (n < (ssize_t)((size_t)chunk_frames * ch * 2)) { lseek(fileno(f), off, SEEK_SET); continue; }
            const int16_t *pcm = (const int16_t *)buf;
            for (int i = 0; i < chunk_frames; i++) {
                mono[i] = (ch == 2) ? (int16_t)(((int)pcm[2 * i] + (int)pcm[2 * i + 1]) >> 1) : pcm[i];
            }
            soundtouch_sim_process(st, mono, chunk_frames);
            c->frames_in += chunk_frames;
            const int64_t due = t0 + (int64_t)((double)c->frames_in * 1e6 / ((double)sr * (double)c->tempo));
            const int64_t wait = due - esp_timer_get_time();
            if (wait >= 10000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
            else if (wait > 0) vTaskDelay(1);
            else if ((uint32_t)(-wait / 1000) > c->max_late_ms) c->max_late_ms = (uint32_t)(-wait / 1000);
        }
    }
    if (f) fclose(f);
    soundtouch_sim_destroy(st);
    heap_caps_free(buf);
    audio_free(mono);
    c->done = true;
    vTaskDelete(nullptr);
}

static void run_cpu_test(const diag_ctx_t &c, uint32_t secs, int sim_core)
{
    out("=== CPU test (%lu s per window) ===", (unsigned long)secs);
    const uint32_t rate_bps = (c.playing && c.sample_rate) ? c.sample_rate * c.channels * c.bytes_per_sample : 192000u;
    const float tempo = (fabsf(c.speed - 1.0f) > 0.05f) ? c.speed : 1.25f;
    out("Playback is %s%s. Simulated 2nd stream: SoundTouch at tempo %.2f on core %d.",
        c.playing ? "RUNNING" : "NOT running (start a song for a realistic test)",
        c.bypass ? ", SoundTouch currently bypassed (player mode)" : "", (double)tempo, sim_core);
    if (c.song_count <= 0) { out("No songs found on the SD card."); return; }

    mem_report("before the test");
    if (report_pipeline(c, "at start") > 0) {
        out("!! a pipeline element is in an ERROR state (a read failed earlier). Stop and restart the song, then run again.");
        return;
    }
    rings_init(c, rate_bps);
    s_play_idx = c.current_idx;
    if (c.playing && c.song_len_s > 0.f) {
        const float left = (c.song_len_s - c.song_pos_s) / (tempo > 0.1f ? tempo : 1.0f);
        out("Current song: %.0f s long, at %.0f s: about %.0f s of playback left (this test needs about %lu s)",
            (double)c.song_len_s, (double)c.song_pos_s, (double)left, (unsigned long)(2 * secs + 6));
        if (left < (float)(2 * secs + 6)) out("WARNING: the song will end during the test - start a longer song and run again.");
    }

    snap_t a = {}, b = {};
    out("  C1: baseline (current playback only)");
    rings_reset();
    snap_take(&a);
    wait_sampling(secs);
    snap_take(&b);
    float base_load = cpu_report(a, b, nullptr, nullptr);
    rings_report();
    snap_free(&a); snap_free(&b);

    /* second stream */
    static sim_ctx sc;
    memset(&sc, 0, sizeof(sc));
    const int ib = (c.song_count > 1 && c.current_idx >= 0) ? ((c.current_idx + 1) % c.song_count) : 0;
    song_path(c, ib, sc.path, sizeof(sc.path));
    sc.tempo = tempo;
    sc.sr = 48000;
    TaskHandle_t th = nullptr;
    if (xTaskCreatePinnedToCore(sim_task, "diag_sim", 12288, &sc, 5, &th, sim_core) != pdPASS) {
        out("ERROR: cannot start the simulated second stream (out of memory?)");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1500));                          /* let it reach steady state */
    if (sc.failed) { out("ERROR: second stream failed to start (%s)", sc.path); return; }

    out("  C2: playback + simulated second stream (%s)", sc.path);
    rings_reset();
    const uint64_t frames0 = sc.frames_in;
    sc.max_late_ms = 0;
    snap_take(&a);
    wait_sampling(secs);
    snap_take(&b);
    float sim_pct = 0.f;
    float load2 = cpu_report(a, b, "diag_sim", &sim_pct);
    const double audio_s = (double)(sc.frames_in - frames0) / (double)sc.sr;
    out("    second stream: %.1f%% of one core for %.1f s of audio (tempo %.2f), max lateness %lu ms",
        (double)sim_pct, audio_s, (double)tempo, (unsigned long)sc.max_late_ms);
    int worst = rings_report();
    snap_free(&a); snap_free(&b);

    sc.stop = true;
    while (!sc.done) vTaskDelay(pdMS_TO_TICKS(50));

    const bool ok = load2 < 85.f && worst >= 25 && sc.max_late_ms < 250;
    out("  => CPU verdict: busiest core %.0f%% -> %.0f%% with a second stream, lowest buffer fill %d%%: %s",
        (double)base_load, (double)load2, worst, ok ? "OK" : "TOO TIGHT / WARNING");
}

/* ── Entry point ───────────────────────────────────────────────────────── */

esp_err_t diag_run(httpd_req_t *req, const char *test, uint32_t secs, int sim_core)
{
    if (s_busy) {
        if (req) httpd_resp_sendstr_chunk(req, "A diagnostics run is already in progress.\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    s_req  = req;
    if (secs < 2)  secs = 2;
    if (secs > 20) secs = 20;
    if (sim_core != 0) sim_core = 1;

    /* Run below the player's own tasks (the SD reader runs at priority 4): the tests must never take the
     * filesystem lock away from playback for long (a 10 s wait makes the reader fail with a timeout). */
    const UBaseType_t old_prio = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, 2);

    diag_ctx_t c = {};
    diag_get_context(&c);

    const bool do_sd  = (strcmp(test, "sd") == 0)  || (strcmp(test, "all") == 0);
    const bool do_cpu = (strcmp(test, "cpu") == 0) || (strcmp(test, "all") == 0);
    const bool do_raw = (strcmp(test, "sdraw") == 0);
    if (!do_sd && !do_cpu && !do_raw) out("Unknown test '%s' (use sd, cpu, sdraw or all)", test);
    if (do_sd)  run_sd_test(c, secs);
    if (do_cpu) run_cpu_test(c, secs, sim_core);
    if (do_raw) run_sdraw_test(c, secs);
    if (do_sd || do_cpu || do_raw) out("=== done ===");

    vTaskPrioritySet(nullptr, old_prio);
    s_req  = nullptr;
    s_busy = false;
    return ESP_OK;
}

#else  /* !HAVE_ADF */

esp_err_t diag_run(httpd_req_t *req, const char *test, uint32_t secs, int sim_core)
{
    (void)test; (void)secs; (void)sim_core;
    if (req) httpd_resp_sendstr_chunk(req, "Diagnostics need the ESP-ADF build.\n");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* HAVE_ADF */
