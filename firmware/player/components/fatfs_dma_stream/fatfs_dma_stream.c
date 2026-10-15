/*
 * fatfs_dma_stream - FAT file reader element for ESP-ADF, tuned for the ESP32-S3 SD card path.
 *
 * Derived from ESP-ADF's fatfs_stream.c (ESPRESSIF MIT License, Copyright (c) 2018 ESPRESSIF SYSTEMS
 * (SHANGHAI) PTE LTD; permission notice as in the original file).  Reader only; open / close / seek-on-open /
 * end-of-file behaviour is unchanged.
 *
 * What is different:
 *
 * 1. Speed.  ESP-ADF allocates the element's buffer in PSRAM (audio_calloc) and fatfs_stream read()s straight
 *    into it.  The ESP32-S3 SDMMC driver cannot DMA into PSRAM (SOC_SDMMC_PSRAM_DMA_CAPABLE is not set), so every
 *    such read is split into one 512-byte sector at a time through a bounce buffer: about 0.7 MB/s on a card
 *    that does 4.7 MB/s.  Here the file is read into an internal, DMA-capable buffer (multi-sector DMA) and then
 *    copied into the element's buffer: measured 2.4 MB/s (8 KB) to 4.4 MB/s (16 KB) for the same card.
 *
 * 2. Crossfade.  fatfs_dma_stream_crossfade() makes the reader keep reading the file that is playing (outgoing)
 *    AND start another file (incoming), and emit the equal-power mix of both PCM streams for a given number of
 *    bytes, after which only the incoming file continues.  Downstream (WAV decoder, SoundTouch, ...) sees one
 *    uninterrupted PCM stream, so nothing has to be stopped or restarted and there is no gap.  Both files must
 *    be canonical 16-bit PCM WAV files (44-byte header) of the same format.
 *
 * 3. Unbounded header (fatfs_dma_stream_set_unbounded_header).  The WAV decoder takes the length from the first
 *    header it sees; a crossfade changes what the stream contains afterwards, so the data-size field of that
 *    header is replaced by a huge value (the stream then ends at end-of-file as usual).
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_common.h"
#include "audio_element.h"
#include "audio_mem.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fatfs_dma_stream.h"

static const char *TAG = "FATFS_DMA";

#define DMA_BUF_MAX   (32 * 1024)      /* upper bound for the internal bounce buffer */
#define WAV_HDR       44u              /* canonical header: the player assumes this everywhere */
#define XF_PATH_MAX   192
#define UNBOUNDED_LEN 0x7FFFF000u

typedef struct {
    bool     is_open;
    int      file;              /* the file being streamed (outgoing file while a crossfade is running) */
    uint32_t main_pos;          /* current file offset of `file` */
    uint8_t *dma;               /* internal, DMA-capable, 32-byte aligned; NULL = fall back to reading directly */
    int      dma_sz;
    int      buf_len;           /* the element's read size */
    bool     unbounded;         /* patch the data size in the stream's WAV header */
    uint32_t limit;             /* end of the PCM of the streamed file (0 = read to end of file) */

    /* crossfade request (written by fatfs_dma_stream_crossfade, consumed in _read) */
    volatile bool xf_pending;
    char     xf_path[XF_PATH_MAX];
    uint32_t xf_fade_bytes;
    uint32_t xf_old_end;        /* file offset where the outgoing PCM ends */
    uint32_t xf_new_start;      /* file offset where the incoming PCM starts */
    uint32_t xf_new_end;        /* file offset where the incoming PCM ends */
    uint8_t  xf_old_mode, xf_new_mode;   /* downmix modes (0 mix, 1 CH1, 2 CH2) of the two songs */
    bool     xf_stereo;         /* pre-downmix both songs to L = R during the fade */

    /* crossfade in progress */
    volatile bool xf_active;
    int      fd_new;
    uint32_t new_pos;
    uint32_t xf_total, xf_done;
    uint8_t *xf_buf;            /* outgoing data of the current block (PSRAM) */
    bool     xf_new_ended;
} fatfs_dma_stream_t;

static char *get_mount_path(char *uri)
{
    /* support format: /sdcard, /spiffs, /storage etc ... */
    if (uri[0] == '/') return uri;

    /* support format: scheme://basepath... */
    char *skip_scheme = strstr(uri, "://");
    if (skip_scheme == NULL) return NULL;
    skip_scheme += 2;

    /* support format: scheme:///basepath... */
    if (skip_scheme[1] == '/') skip_scheme++;

    return skip_scheme;
}

/* read up to `len` bytes from fd into dst through the internal DMA buffer; returns bytes read (0 = EOF), -1 on error */
static int read_into(fatfs_dma_stream_t *fs, int fd, uint8_t *dst, int len)
{
    int total = 0;
    if (!fs->dma) return read(fd, dst, len);          /* slow single-sector path */

    while (total < len) {
        int want = len - total;
        if (want > fs->dma_sz) want = fs->dma_sz;
        int got = read(fd, fs->dma, want);
        if (got < 0) {
            ESP_LOGE(TAG, "The error is happened in reading data. Error message: %s", strerror(errno));
            return total > 0 ? total : -1;           /* report the error only if nothing was read */
        }
        if (got == 0) break;                          /* end of file */
        memcpy(dst + total, fs->dma, got);
        total += got;
        if (got < want) break;                        /* short read: end of file */
    }
    return total;
}

static void xf_cleanup(fatfs_dma_stream_t *fs)
{
    if (fs->fd_new > 0) { close(fs->fd_new); }
    fs->fd_new = -1;
    if (fs->xf_buf) { audio_free(fs->xf_buf); fs->xf_buf = NULL; }
    fs->xf_active = false;
    fs->xf_pending = false;
    fs->xf_new_ended = false;
}

static esp_err_t _open(audio_element_handle_t self)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(self);

    if (fs->is_open) {
        ESP_LOGE(TAG, "already opened");
        return ESP_FAIL;
    }

    audio_element_info_t info;
    char *uri = audio_element_get_uri(self);
    if (uri == NULL) {
        ESP_LOGE(TAG, "Error, uri is not set");
        return ESP_FAIL;
    }
    char *path = get_mount_path(uri);
    if (path == NULL) {
        ESP_LOGE(TAG, "Error, Invalid file format (%s).", uri);
        return ESP_FAIL;
    }

    audio_element_getinfo(self, &info);

    fs->file = open(path, O_RDONLY);
    if (fs->file == -1) {
        ESP_LOGE(TAG, "Failed to open. File name: %s, error message: %s, line: %d", path, strerror(errno), __LINE__);
        return ESP_FAIL;
    }
    struct stat siz = { 0 };
    stat(path, &siz);
    info.total_bytes = siz.st_size;
    ESP_LOGI(TAG, "File size: %d byte, file position: %d", (int)siz.st_size, (int)info.byte_pos);
    fs->main_pos = 0;
    if (info.byte_pos > 0) {
        if (lseek(fs->file, info.byte_pos, SEEK_SET) < 0) {
            ESP_LOGE(TAG, "Error seek file. Error message: %s, line: %d", strerror(errno), __LINE__);
            close(fs->file);
            return ESP_FAIL;
        }
        fs->main_pos = (uint32_t)info.byte_pos;
    }
    xf_cleanup(fs);
    fs->is_open = true;
    return audio_element_set_total_bytes(self, info.total_bytes);
}

/* ---- crossfade ------------------------------------------------------------------------------------------ */

/* Runs in the reader task at the start of a read: open the incoming file and switch to mixing. */
static void xf_start(fatfs_dma_stream_t *fs)
{
    fs->xf_pending = false;
    fs->fd_new = open(fs->xf_path, O_RDONLY);
    if (fs->fd_new < 0) {
        ESP_LOGE(TAG, "crossfade: cannot open %s (%s) - continuing with the current file", fs->xf_path, strerror(errno));
        fs->fd_new = -1;
        return;
    }
    if (lseek(fs->fd_new, fs->xf_new_start, SEEK_SET) < 0) {
        ESP_LOGE(TAG, "crossfade: seek failed");
        close(fs->fd_new); fs->fd_new = -1;
        return;
    }
    fs->xf_buf = audio_malloc(fs->buf_len);
    if (!fs->xf_buf) {
        ESP_LOGE(TAG, "crossfade: out of memory");
        close(fs->fd_new); fs->fd_new = -1;
        return;
    }
    fs->new_pos = fs->xf_new_start;
    fs->xf_total = fs->xf_fade_bytes;
    fs->xf_done = 0;
    fs->xf_new_ended = false;
    fs->xf_active = true;
    ESP_LOGI(TAG, "crossfade started: %u bytes, outgoing at %u / %u, incoming %s",
             (unsigned)fs->xf_total, (unsigned)fs->main_pos, (unsigned)fs->xf_old_end, fs->xf_path);
}

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

/* Stereo -> mono as the SoundTouch element does it (mode 0: equal-power mix, 1: CH1, 2: CH2), written to both
 * channels.  The player then runs SoundTouch in CH1 mode (identity) while the crossfade passes through. */
static void predownmix(int16_t *b, int samples, uint8_t mode)
{
    for (int i = 0; i + 1 < samples; i += 2) {
        int32_t l = b[i], r = b[i + 1];
        int16_t w = (mode == 1) ? (int16_t)l : (mode == 2) ? (int16_t)r : sat16(((l + r) * 23170) >> 15);
        b[i] = w; b[i + 1] = w;
    }
}

/* One block of the crossfade: equal-power mix of the incoming (new) and outgoing (old) PCM. */
static int xf_read(audio_element_handle_t self, fatfs_dma_stream_t *fs, uint8_t *buffer, int len)
{
    int want = len;
    const uint32_t left = fs->xf_total - fs->xf_done;
    if ((uint32_t)want > left) want = (int)left;
    want &= ~3;                                       /* whole 16-bit stereo frames */
    if (want <= 0) want = 4;

    /* incoming file -> buffer */
    int room_new = (fs->xf_new_end > fs->new_pos) ? (int)(fs->xf_new_end - fs->new_pos) : 0;
    int n_new = 0;
    if (room_new > 0) {
        n_new = read_into(fs, fs->fd_new, buffer, want < room_new ? want : room_new);
        if (n_new < 0) n_new = 0;
    }
    n_new &= ~3;
    fs->new_pos += (uint32_t)n_new;

    /* outgoing file -> xf_buf (zeros once it has ended) */
    int room_old = (fs->xf_old_end > fs->main_pos) ? (int)(fs->xf_old_end - fs->main_pos) : 0;
    int n_old = 0;
    if (room_old > 0) {
        n_old = read_into(fs, fs->file, fs->xf_buf, want < room_old ? want : room_old);
        if (n_old < 0) n_old = 0;
    }
    n_old &= ~3;
    fs->main_pos += (uint32_t)n_old;

    const int out = n_new;                            /* the incoming file defines the stream length */
    if (fs->xf_stereo) {
        predownmix((int16_t *)buffer, out / 2, fs->xf_new_mode);
        predownmix((int16_t *)fs->xf_buf, n_old / 2, fs->xf_old_mode);
    }
    if (out < want) fs->xf_new_ended = true;          /* incoming file ended during the fade */

    /* gains: linear in time between the block start and end, equal-power law (cos / sin) */
    const float t0 = (float)fs->xf_done / (float)fs->xf_total;
    const float t1 = (float)(fs->xf_done + (uint32_t)(out > 0 ? out : 4)) / (float)fs->xf_total;
    const float gn0 = sinf(t0 * 1.57079633f), gn1 = sinf(t1 * 1.57079633f);
    const float go0 = cosf(t0 * 1.57079633f), go1 = cosf(t1 * 1.57079633f);
    const int samples = out / 2;
    int16_t *nb = (int16_t *)buffer;
    const int16_t *ob = (const int16_t *)fs->xf_buf;
    const int old_samples = n_old / 2;
    for (int i = 0; i < samples; i += 2) {            /* one stereo frame per step: same gain for both channels */
        const float f = (samples > 2) ? (float)i / (float)(samples - 2) : 0.0f;
        const float gn = gn0 + (gn1 - gn0) * f;
        const float go = go0 + (go1 - go0) * f;
        for (int c = 0; c < 2 && i + c < samples; c++) {
            const int32_t o = (i + c < old_samples) ? ob[i + c] : 0;
            nb[i + c] = sat16((int32_t)((float)nb[i + c] * gn + (float)o * go));
        }
    }

    fs->xf_done += (uint32_t)out;
    if (fs->xf_new_ended || fs->xf_done >= fs->xf_total) {
        /* fade over: from now on only the incoming file is streamed */
        close(fs->file);
        fs->file = fs->fd_new;
        fs->fd_new = -1;
        fs->main_pos = fs->new_pos;
        fs->limit = fs->xf_new_end;
        ESP_LOGI(TAG, "crossfade finished at file offset %u", (unsigned)fs->main_pos);
        xf_cleanup(fs);
    }
    if (out > 0) audio_element_update_byte_pos(self, out);
    return out;                                       /* 0 = incoming file already at its end: end of stream */
}

static void patch_unbounded_header(fatfs_dma_stream_t *fs, uint8_t *buffer, int got, uint32_t pos_before)
{
    if (!fs->unbounded || pos_before != 0 || got < (int)WAV_HDR) return;
    if (memcmp(buffer, "RIFF", 4) != 0 || memcmp(buffer + 36, "data", 4) != 0) return;
    const uint32_t riff = UNBOUNDED_LEN + 36u, data = UNBOUNDED_LEN;
    memcpy(buffer + 4, &riff, 4);
    memcpy(buffer + 40, &data, 4);
}

static int _read(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(self);

    if (fs->xf_pending && !fs->xf_active) xf_start(fs);
    if (fs->xf_active) return xf_read(self, fs, (uint8_t *)buffer, len);

    const uint32_t pos_before = fs->main_pos;
    if (fs->limit) {                                  /* do not stream anything behind the PCM data (trailing chunks) */
        if (fs->main_pos >= fs->limit) return 0;
        if ((uint32_t)len > fs->limit - fs->main_pos) len = (int)(fs->limit - fs->main_pos);
    }
    int total = read_into(fs, fs->file, (uint8_t *)buffer, len);

    if (total == 0) {
        ESP_LOGW(TAG, "No more data, ret:%d", total);
    } else if (total > 0) {
        patch_unbounded_header(fs, (uint8_t *)buffer, total, pos_before);
        fs->main_pos += (uint32_t)total;
        audio_element_update_byte_pos(self, total);
    }
    return total;
}

static int _process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r_size = audio_element_input(self, in_buffer, in_len);
    int w_size = 0;
    if (r_size > 0) {
        w_size = audio_element_output(self, in_buffer, r_size);
    } else {
        w_size = r_size;
    }
    return w_size;
}

static esp_err_t _close(audio_element_handle_t self)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(self);

    if (fs->is_open) {
        close(fs->file);
        fs->is_open = false;
    }
    xf_cleanup(fs);
    if (AEL_STATE_PAUSED != audio_element_get_state(self)) {
        audio_element_report_info(self);
        audio_element_set_byte_pos(self, 0);
    }
    return ESP_OK;
}

static esp_err_t _destroy(audio_element_handle_t self)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(self);
    if (fs) {
        xf_cleanup(fs);
        heap_caps_free(fs->dma);
        audio_free(fs);
    }
    return ESP_OK;
}

audio_element_handle_t fatfs_dma_stream_init(fatfs_stream_cfg_t *config)
{
    if (!config || config->type != AUDIO_STREAM_READER) {
        ESP_LOGE(TAG, "reader streams only");
        return NULL;
    }

    fatfs_dma_stream_t *fs = audio_calloc(1, sizeof(fatfs_dma_stream_t));
    AUDIO_MEM_CHECK(TAG, fs, return NULL);
    fs->fd_new = -1;

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open        = _open;
    cfg.close       = _close;
    cfg.process     = _process;
    cfg.destroy     = _destroy;
    cfg.read        = _read;
    cfg.task_stack  = config->task_stack;
    cfg.task_prio   = config->task_prio;
    cfg.task_core   = config->task_core;
    cfg.out_rb_size = config->out_rb_size;
    cfg.buffer_len  = config->buf_sz;
    cfg.stack_in_ext = config->ext_stack;
    if (cfg.buffer_len == 0) {
        cfg.buffer_len = FATFS_STREAM_BUF_SIZE;
    }
    cfg.tag = "file";
    fs->buf_len = cfg.buffer_len;

    /* internal DMA bounce buffer, as large as one read but at most DMA_BUF_MAX */
    fs->dma_sz = cfg.buffer_len < DMA_BUF_MAX ? cfg.buffer_len : DMA_BUF_MAX;
    fs->dma_sz &= ~511;                                   /* whole sectors */
    if (fs->dma_sz < 512) fs->dma_sz = 512;
    fs->dma = heap_caps_aligned_alloc(32, fs->dma_sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fs->dma) {
        ESP_LOGW(TAG, "no internal DMA buffer (%d B): SD reads will be slow", fs->dma_sz);
    }

    audio_element_handle_t el = audio_element_init(&cfg);
    AUDIO_MEM_CHECK(TAG, el, goto init_fail);
    audio_element_setdata(el, fs);
    ESP_LOGI(TAG, "reader ready: %d B reads via internal DMA buffer%s", fs->dma_sz, fs->dma ? "" : " (UNAVAILABLE)");
    return el;

init_fail:
    heap_caps_free(fs->dma);
    audio_free(fs);
    return NULL;
}

/* ---- public control API ---------------------------------------------------------------------------------- */

void fatfs_dma_stream_set_unbounded_header(audio_element_handle_t el, bool on)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(el);
    if (fs) fs->unbounded = on;
}

void fatfs_dma_stream_set_limit(audio_element_handle_t el, uint32_t end_off)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(el);
    if (fs) fs->limit = end_off;
}

esp_err_t fatfs_dma_stream_crossfade(audio_element_handle_t el, const char *new_path, uint32_t fade_bytes,
                                     uint32_t old_end_off, uint32_t new_start_off, uint32_t new_end_off,
                                     uint8_t old_mode, uint8_t new_mode, bool stereo)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(el);
    if (!fs || !new_path || strlen(new_path) >= XF_PATH_MAX || fade_bytes < 4096) return ESP_ERR_INVALID_ARG;
    if (!fs->is_open || fs->xf_active || fs->xf_pending) return ESP_ERR_INVALID_STATE;
    if (audio_element_get_state(el) != AEL_STATE_RUNNING) return ESP_ERR_INVALID_STATE;
    /* too close to the end of the outgoing file: it has probably been read ahead completely already */
    if (fs->main_pos + (uint32_t)fs->buf_len * 2u >= old_end_off) return ESP_ERR_INVALID_STATE;

    strlcpy(fs->xf_path, new_path, sizeof(fs->xf_path));
    fs->xf_fade_bytes = fade_bytes & ~3u;
    fs->xf_old_end    = old_end_off;
    fs->xf_new_start  = new_start_off;
    fs->xf_new_end    = new_end_off;
    fs->xf_old_mode   = old_mode;
    fs->xf_new_mode   = new_mode;
    fs->xf_stereo     = stereo;
    __atomic_store_n(&fs->xf_pending, true, __ATOMIC_RELEASE);
    return ESP_OK;
}

bool fatfs_dma_stream_crossfade_busy(audio_element_handle_t el)
{
    fatfs_dma_stream_t *fs = (fatfs_dma_stream_t *)audio_element_getdata(el);
    return fs && (fs->xf_pending || fs->xf_active);
}
