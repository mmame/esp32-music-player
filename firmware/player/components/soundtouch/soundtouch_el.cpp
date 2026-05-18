/**
 * @file soundtouch_el.cpp
 * @brief ADF audio element wrapper for SoundTouch time-stretching.
 *
 * The ADF pipeline carries signed 16-bit interleaved PCM.
 * SoundTouch is compiled with SOUNDTOUCH_INTEGER_SAMPLES=1 so
 * SAMPLETYPE = short (int16_t) – no float conversion needed.
 */

#include "soundtouch_el.h"

#include "audio_element.h"
#include "audio_mem.h"
#include "audio_error.h"
#include "esp_log.h"

#include "SoundTouch.h"

#include <new>      /* std::nothrow */
#include <string.h>

static const char *TAG = "SOUNDTOUCH";

/* Frames fed to SoundTouch per _process() call.
 * SoundTouch auto-tunes its sequence length (~82 ms = 3617 frames at 44100 Hz).
 * At 2.0x tempo the input advance per batch = 2.0 x (3617-353) = 6528 frames.
 * ST_CHUNK_FRAMES must exceed this so every putSamples() call yields output
 * immediately, preventing i2s ring-buffer starvation (stutter). 16384 > 6528. */
static constexpr int ST_CHUNK_FRAMES = 16384;

/* Frames requested per receiveSamples() call inside drain().
 * Kept smaller than ST_CHUNK_FRAMES to limit stack/buffer pressure. */
static constexpr int ST_DRAIN_FRAMES = 4096;

/* -- Internal context ------------------------------------------------------ */

struct StCtx {
    soundtouch::SoundTouch *st;
    int            samplerate;
    int            channels;
    volatile float target_tempo;   /* written by any task, read by element task */
    float          applied_tempo;  /* last value actually sent to SoundTouch    */
    volatile bool  bypass;         /* true = passthrough, no SoundTouch         */
    bool           prev_bypass;    /* previous bypass state for transition detect */

    /* int16 buffers - interface to ADF ring buffers and SoundTouch        */
    int16_t *pcm_in;   /* ST_CHUNK_FRAMES x channels                       */
    int16_t *pcm_out;  /* ST_DRAIN_FRAMES x channels                       */
};

/* -- Helpers --------------------------------------------------------------- */

static inline StCtx *ctx_of(audio_element_handle_t self)
{
    return static_cast<StCtx *>(audio_element_getdata(self));
}

/** Receive all frames currently available in SoundTouch and write to the
 *  downstream ring buffer.  SAMPLETYPE = short so pcm_out is used directly. */
static void drain(audio_element_handle_t self, StCtx *ctx)
{
    uint frames;
    do {
        frames = ctx->st->receiveSamples(ctx->pcm_out, (uint)ST_DRAIN_FRAMES);
        if (frames > 0) {
            audio_element_output(self,
                                 reinterpret_cast<char *>(ctx->pcm_out),
                                 (int)(frames * (uint)ctx->channels) * (int)sizeof(int16_t));
        }
    } while (frames > 0);
}

/* -- ADF element callbacks ------------------------------------------------- */

static esp_err_t _open(audio_element_handle_t self)
{
    ctx_of(self)->st->clear();
    return ESP_OK;
}

static esp_err_t _close(audio_element_handle_t self)
{
    (void)self;
    return ESP_OK;
}

static audio_element_err_t _process(audio_element_handle_t self,
                                    char * /*in_buf*/, int /*in_size*/)
{
    StCtx *ctx = ctx_of(self);

    /* Detect bypass state transitions. */
    bool cur_bypass = ctx->bypass;
    if (cur_bypass != ctx->prev_bypass) {
        if (!cur_bypass) {
            /* Leaving bypass: clear SoundTouch to avoid stale lookahead data. */
            ctx->st->clear();
        }
        ctx->prev_bypass = cur_bypass;
    }

    /* When bypass is active, copy PCM straight through – zero SoundTouch involvement. */
    if (cur_bypass) {
        int rb_bytes = ST_CHUNK_FRAMES * ctx->channels * (int)sizeof(int16_t);
        int bytes_in = audio_element_input(self,
                                           reinterpret_cast<char *>(ctx->pcm_in),
                                           rb_bytes);
        if (bytes_in > 0) {
            audio_element_output(self,
                                 reinterpret_cast<char *>(ctx->pcm_in),
                                 bytes_in);
        }
        return static_cast<audio_element_err_t>(bytes_in);
    }

    /* Apply any pending tempo change before processing this chunk. */
    float tgt = ctx->target_tempo;
    if (tgt != ctx->applied_tempo) {
        ctx->applied_tempo = tgt;
        ctx->st->setTempo((double)tgt);
    }

    /* Pull one chunk of int16 PCM from the upstream ring buffer. */
    int rb_bytes = ST_CHUNK_FRAMES * ctx->channels * (int)sizeof(int16_t);
    int bytes_in = audio_element_input(self,
                                       reinterpret_cast<char *>(ctx->pcm_in),
                                       rb_bytes);
    if (bytes_in <= 0) {
        if (bytes_in == AEL_IO_DONE) {
            /* Flush SoundTouch's internal lookahead and emit remaining frames. */
            ctx->st->flush();
            drain(self, ctx);
        }
        return static_cast<audio_element_err_t>(bytes_in);
    }

    /* Feed int16 PCM directly to SoundTouch (SAMPLETYPE = short). */
    int frames_in = bytes_in / (ctx->channels * (int)sizeof(int16_t));
    ctx->st->putSamples(ctx->pcm_in, (uint)frames_in);

    /* Drain all available output. */
    drain(self, ctx);

    return static_cast<audio_element_err_t>(bytes_in);
}

static esp_err_t _destroy(audio_element_handle_t self)
{
    StCtx *ctx = ctx_of(self);
    if (ctx) {
        delete ctx->st;
        audio_free(ctx->pcm_in);
        audio_free(ctx->pcm_out);
        audio_free(ctx);
    }
    return ESP_OK;
}

/* -- Public API ----------------------------------------------------------- */

esp_err_t soundtouch_el_set_tempo(audio_element_handle_t self, float tempo)
{
    StCtx *ctx = ctx_of(self);
    if (!ctx || tempo <= 0.0f) return ESP_ERR_INVALID_ARG;
    /* volatile write - effectively atomic on 32-bit aligned Xtensa. */
    ctx->target_tempo = tempo;
    return ESP_OK;
}

esp_err_t soundtouch_el_set_bypass(audio_element_handle_t self, bool bypass)
{
    StCtx *ctx = ctx_of(self);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    /* volatile write - effectively atomic on 32-bit aligned Xtensa. */
    ctx->bypass = bypass;
    return ESP_OK;
}

audio_element_handle_t soundtouch_el_init(const soundtouch_el_cfg_t *cfg)
{
    StCtx *ctx = static_cast<StCtx *>(audio_calloc(1, sizeof(StCtx)));
    AUDIO_MEM_CHECK(TAG, ctx, return NULL);

    ctx->samplerate    = cfg->samplerate;
    ctx->channels      = cfg->channels;
    ctx->applied_tempo = cfg->tempo;
    ctx->target_tempo  = cfg->tempo;
    ctx->bypass        = false;
    ctx->prev_bypass   = false;

    /* int16 PCM buffers (may live in PSRAM via audio_calloc). */
    ctx->pcm_in  = static_cast<int16_t *>(
        audio_calloc(ST_CHUNK_FRAMES   * cfg->channels, sizeof(int16_t)));
    ctx->pcm_out = static_cast<int16_t *>(
        audio_calloc(ST_DRAIN_FRAMES   * cfg->channels, sizeof(int16_t)));

    if (!ctx->pcm_in || !ctx->pcm_out) {
        ESP_LOGE(TAG, "OOM allocating I/O buffers");
        goto fail;
    }

    /* SoundTouch instance. */
    ctx->st = new(std::nothrow) soundtouch::SoundTouch();
    if (!ctx->st) { ESP_LOGE(TAG, "OOM: SoundTouch()"); goto fail; }

    ctx->st->setSampleRate((uint)cfg->samplerate);
    ctx->st->setChannels((uint)cfg->channels);
    ctx->st->setTempo((double)cfg->tempo);

    /* Quality settings – let SoundTouch auto-tune sequence/seek/overlap for
     * the best possible quality.  Stutter prevention is achieved by setting
     * ST_CHUNK_FRAMES large enough (16384) that even the maximum auto-tuned
     * input advance at 2.0x tempo (6528 frames) fits in a single call. */
    ctx->st->setSetting(SETTING_USE_AA_FILTER,    1);
    ctx->st->setSetting(SETTING_AA_FILTER_LENGTH, 32);  /* 32-tap AA filter */
    ctx->st->setSetting(SETTING_USE_QUICKSEEK,    1);   /* QuickSeek ON: ~4x faster cross-corr */
    ctx->st->setSetting(SETTING_SEQUENCE_MS,      0);   /* auto-tune             */
    ctx->st->setSetting(SETTING_SEEKWINDOW_MS,    0);   /* auto-tune             */
    ctx->st->setSetting(SETTING_OVERLAP_MS,       0);   /* auto-tune             */

    {
        audio_element_cfg_t el_cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
        el_cfg.open         = _open;
        el_cfg.close        = _close;
        el_cfg.process      = _process;
        el_cfg.destroy      = _destroy;
        el_cfg.task_stack   = cfg->task_stack;
        el_cfg.task_prio    = cfg->task_prio;
        el_cfg.task_core    = cfg->task_core;
        el_cfg.stack_in_ext = cfg->stack_in_ext;
        el_cfg.out_rb_size  = cfg->out_rb_size;
        el_cfg.buffer_len   = 0;        /* element manages its own buffers */
        el_cfg.tag          = "soundtouch";

        audio_element_handle_t el = audio_element_init(&el_cfg);
        if (!el) { ESP_LOGE(TAG, "audio_element_init failed"); goto fail; }
        audio_element_setdata(el, ctx);
        ESP_LOGI(TAG, "SoundTouch element ready  sr=%d  ch=%d  tempo=%.2f",
                 cfg->samplerate, cfg->channels, (double)cfg->tempo);
        return el;
    }

fail:
    if (ctx) {
        delete ctx->st;
        audio_free(ctx->pcm_in);
        audio_free(ctx->pcm_out);
        audio_free(ctx);
    }
    return NULL;
}
