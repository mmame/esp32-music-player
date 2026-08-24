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
#include <math.h>

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
    int            in_channels;
    int            st_channels;
    volatile float target_tempo;   /* written by any task, read by element task */
    float          applied_tempo;  /* last value actually sent to SoundTouch    */
    volatile bool  bypass;         /* true = passthrough, no SoundTouch         */
    bool           prev_bypass;    /* previous bypass state for transition detect */

    volatile float pitch_influence;         /* 0.0 = time-stretch, 1.0 = tape effect  */
    float          applied_pitch_influence; /* last value applied to SoundTouch        */

    volatile uint8_t  downmix_mode_target;  /* 0=mix,1=left,2=right */
    uint8_t           downmix_mode_active;
    volatile uint16_t downmix_fade_ms;
    volatile bool     downmix_fade_armed;
    bool              downmix_fading;
    float             dm_cur_l;
    float             dm_cur_r;
    float             dm_step_l;
    float             dm_step_r;
    uint32_t          dm_fade_left;

    /* int16 buffers - interface to ADF ring buffers and SoundTouch        */
    int16_t *pcm_in;   /* ST_CHUNK_FRAMES x in_channels                    */
    int16_t *mono_in;  /* ST_CHUNK_FRAMES x 1                              */
    int16_t *pcm_out;  /* ST_DRAIN_FRAMES x st_channels                    */
};

/* -- Helpers --------------------------------------------------------------- */

static inline StCtx *ctx_of(audio_element_handle_t self)
{
    return static_cast<StCtx *>(audio_element_getdata(self));
}

static inline void mode_to_gain(uint8_t mode, float *gl, float *gr)
{
    if (mode == 1u) {
        *gl = 1.0f; *gr = 0.0f;
    } else if (mode == 2u) {
        *gl = 0.0f; *gr = 1.0f;
    } else {
        *gl = 0.5f; *gr = 0.5f;
    }
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
                                 (int)(frames * (uint)ctx->st_channels) * (int)sizeof(int16_t));
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

    int rb_bytes = ST_CHUNK_FRAMES * ctx->in_channels * (int)sizeof(int16_t);
    int bytes_in = audio_element_input(self,
                                       reinterpret_cast<char *>(ctx->pcm_in),
                                       rb_bytes);
    if (bytes_in <= 0) {
        if (!cur_bypass && bytes_in == AEL_IO_DONE) {
            ctx->st->flush();
            drain(self, ctx);
        }
        return static_cast<audio_element_err_t>(bytes_in);
    }

    int frames_in = bytes_in / (ctx->in_channels * (int)sizeof(int16_t));

    /* Detect downmix mode changes and optionally start fade. */
    uint8_t mode = ctx->downmix_mode_target;
    if (mode > 2u) mode = 0u;
    if (mode != ctx->downmix_mode_active) {
        float to_l = 0.5f, to_r = 0.5f;
        mode_to_gain(mode, &to_l, &to_r);
        if (ctx->downmix_fade_armed && ctx->downmix_fade_ms > 0u) {
            uint32_t fs = ((uint32_t)ctx->samplerate * (uint32_t)ctx->downmix_fade_ms) / 1000u;
            if (fs > 0u) {
                ctx->dm_fade_left = fs;
                ctx->dm_step_l = (to_l - ctx->dm_cur_l) / (float)fs;
                ctx->dm_step_r = (to_r - ctx->dm_cur_r) / (float)fs;
                ctx->downmix_fading = true;
            } else {
                ctx->dm_cur_l = to_l;
                ctx->dm_cur_r = to_r;
                ctx->downmix_fading = false;
                ctx->dm_fade_left = 0u;
            }
        } else {
            ctx->dm_cur_l = to_l;
            ctx->dm_cur_r = to_r;
            ctx->downmix_fading = false;
            ctx->dm_fade_left = 0u;
        }
        ctx->downmix_mode_active = mode;
        ctx->downmix_fade_armed = false;
    }

    /* Stereo->mono downmix. */
    if (ctx->in_channels <= 1) {
        memcpy(ctx->mono_in, ctx->pcm_in, (size_t)frames_in * sizeof(int16_t));
    } else if (!ctx->downmix_fading) {
        if (ctx->downmix_mode_active == 1u) {
            for (int i = 0; i < frames_in; ++i) ctx->mono_in[i] = ctx->pcm_in[i * 2 + 0];
        } else if (ctx->downmix_mode_active == 2u) {
            for (int i = 0; i < frames_in; ++i) ctx->mono_in[i] = ctx->pcm_in[i * 2 + 1];
        } else {
            for (int i = 0; i < frames_in; ++i) {
                int32_t l = ctx->pcm_in[i * 2 + 0];
                int32_t r = ctx->pcm_in[i * 2 + 1];
                ctx->mono_in[i] = (int16_t)((l + r) >> 1);
            }
        }
    } else {
        for (int i = 0; i < frames_in; ++i) {
            float l = (float)ctx->pcm_in[i * 2 + 0];
            float r = (float)ctx->pcm_in[i * 2 + 1];
            float y = l * ctx->dm_cur_l + r * ctx->dm_cur_r;
            if (y > 32767.0f) y = 32767.0f;
            if (y < -32768.0f) y = -32768.0f;
            ctx->mono_in[i] = (int16_t)y;
            if (ctx->dm_fade_left > 0u) {
                ctx->dm_cur_l += ctx->dm_step_l;
                ctx->dm_cur_r += ctx->dm_step_r;
                ctx->dm_fade_left--;
                if (ctx->dm_fade_left == 0u) ctx->downmix_fading = false;
            }
        }
    }

    /* When bypass is active, output downmixed PCM directly. */
    if (cur_bypass) {
        int out_bytes = frames_in * (int)sizeof(int16_t);
        int ret = audio_element_output(self,
                                       reinterpret_cast<char *>(ctx->mono_in),
                                       out_bytes);
        return static_cast<audio_element_err_t>(ret);
    }

    /* Apply any pending tempo / rate change before processing this chunk. */
    float tgt   = ctx->target_tempo;
    float alpha = ctx->pitch_influence;
    if (alpha < 0.0f) alpha = 0.0f; else if (alpha > 1.0f) alpha = 1.0f;
    bool changed = (tgt != ctx->applied_tempo || alpha != ctx->applied_pitch_influence);
    if (changed) {
        if (alpha != ctx->applied_pitch_influence) {
            ctx->st->clear(); /* flush lookahead on influence change */
            ctx->applied_pitch_influence = alpha;
        }
        ctx->applied_tempo = tgt;
        float rate  = powf(tgt, alpha);
        float tempo = powf(tgt, 1.0f - alpha);
        ctx->st->setRate((double)rate);
        ctx->st->setTempo((double)tempo);
    }

    /* Pull one chunk of int16 PCM from the upstream ring buffer. */
    /* Feed downmixed mono PCM to SoundTouch. */
    ctx->st->putSamples(ctx->mono_in, (uint)frames_in);

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
        audio_free(ctx->mono_in);
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

esp_err_t soundtouch_el_set_pitch_influence(audio_element_handle_t self, float pitch_influence)
{
    StCtx *ctx = ctx_of(self);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (pitch_influence < 0.0f) pitch_influence = 0.0f;
    else if (pitch_influence > 1.0f) pitch_influence = 1.0f;
    ctx->pitch_influence = pitch_influence;
    return ESP_OK;
}

esp_err_t soundtouch_el_set_downmix_mode(audio_element_handle_t self, uint8_t mode)
{
    StCtx *ctx = ctx_of(self);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    if (mode > 2u) mode = 0u;
    ctx->downmix_mode_target = mode;
    return ESP_OK;
}

esp_err_t soundtouch_el_set_downmix_fade_ms(audio_element_handle_t self, uint16_t fade_ms)
{
    StCtx *ctx = ctx_of(self);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    ctx->downmix_fade_ms = fade_ms;
    return ESP_OK;
}

esp_err_t soundtouch_el_arm_downmix_fade(audio_element_handle_t self, bool armed)
{
    StCtx *ctx = ctx_of(self);
    if (!ctx) return ESP_ERR_INVALID_ARG;
    ctx->downmix_fade_armed = armed;
    return ESP_OK;
}

audio_element_handle_t soundtouch_el_init(const soundtouch_el_cfg_t *cfg)
{
    StCtx *ctx = static_cast<StCtx *>(audio_calloc(1, sizeof(StCtx)));
    AUDIO_MEM_CHECK(TAG, ctx, return NULL);

    ctx->samplerate    = cfg->samplerate;
    ctx->in_channels   = cfg->channels;
    ctx->st_channels   = 1;
    ctx->applied_tempo      = cfg->tempo;
    ctx->target_tempo       = cfg->tempo;
    ctx->bypass             = false;
    ctx->prev_bypass        = false;
    ctx->pitch_influence         = 0.0f;
    ctx->applied_pitch_influence = 0.0f;
    ctx->downmix_mode_target     = 0u;
    ctx->downmix_mode_active     = 0u;
    ctx->downmix_fade_ms         = 1000u;
    ctx->downmix_fade_armed      = false;
    ctx->downmix_fading          = false;
    ctx->dm_cur_l                = 0.5f;
    ctx->dm_cur_r                = 0.5f;
    ctx->dm_step_l               = 0.0f;
    ctx->dm_step_r               = 0.0f;
    ctx->dm_fade_left            = 0u;

    /* int16 PCM buffers (may live in PSRAM via audio_calloc). */
    ctx->pcm_in  = static_cast<int16_t *>(
        audio_calloc(ST_CHUNK_FRAMES   * cfg->channels, sizeof(int16_t)));
    ctx->mono_in = static_cast<int16_t *>(
        audio_calloc(ST_CHUNK_FRAMES, sizeof(int16_t)));
    ctx->pcm_out = static_cast<int16_t *>(
        audio_calloc(ST_DRAIN_FRAMES   * ctx->st_channels, sizeof(int16_t)));

    if (!ctx->pcm_in || !ctx->mono_in || !ctx->pcm_out) {
        ESP_LOGE(TAG, "OOM allocating I/O buffers");
        goto fail;
    }

    /* SoundTouch instance. */
    ctx->st = new(std::nothrow) soundtouch::SoundTouch();
    if (!ctx->st) { ESP_LOGE(TAG, "OOM: SoundTouch()"); goto fail; }

    ctx->st->setSampleRate((uint)cfg->samplerate);
    ctx->st->setChannels((uint)ctx->st_channels);
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
        audio_free(ctx->mono_in);
        audio_free(ctx->pcm_out);
        audio_free(ctx);
    }
    return NULL;
}
