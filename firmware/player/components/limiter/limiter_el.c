#include "limiter_el.h"

#include <string.h>
#include "esp_log.h"
#include "audio_error.h"
#include "audio_mem.h"
#include "esp_ae_drc.h"

static const char *TAG = "limiter_el";

#define LIM_BUF_BYTES     2048
#define LIM_CEILING_DB    1.0f   /* headroom below full scale, absorbs attack overshoot */
#define LIM_ATTACK_MS     1
#define LIM_RELEASE_MS    100

typedef struct {
    esp_ae_drc_handle_t drc;
    int16_t            *buf;
    int                 samplerate;
    int                 channels;
    volatile int8_t     gain_req;      /* written by any task            */
    int8_t              gain_applied;  /* owned by the element task      */
} lim_ctx_t;

static inline lim_ctx_t *ctx_of(audio_element_handle_t self)
{
    return (lim_ctx_t *)audio_element_getdata(self);
}

static inline int8_t clamp_gain(int g)
{
    if (g > LIMITER_EL_MAX_GAIN_DB) g = LIMITER_EL_MAX_GAIN_DB;
    if (g < LIMITER_EL_MIN_GAIN_DB) g = LIMITER_EL_MIN_GAIN_DB;
    return (int8_t)g;
}

/* Fill the DRC curve for gain_db; returns the number of points.
 * gain > 0: limiter, knee at -(gain+ceiling) so make-up lands on the -1 dBFS ceiling.
 * gain <= 0: identity curve (pure attenuation, nothing to limit). */
static uint8_t curve_for(int8_t gain_db, esp_ae_drc_curve_point pts[3])
{
    pts[0].x = -100.0f; pts[0].y = -100.0f;
    if (gain_db > 0) {
        float t = -((float)gain_db + LIM_CEILING_DB);
        pts[1].x = t;    pts[1].y = t;
        pts[2].x = 0.0f; pts[2].y = t;
        return 3;
    }
    pts[1].x = 0.0f; pts[1].y = 0.0f;
    return 2;
}

/* Push curve + make-up gain for a non-zero gain_db into the DRC. */
static esp_err_t apply_gain(lim_ctx_t *c, int8_t gain_db)
{
    esp_ae_drc_curve_point pts[3];
    uint8_t n = curve_for(gain_db, pts);
    if (esp_ae_drc_set_curve_points(c->drc, pts, n) != ESP_AE_ERR_OK) return ESP_FAIL;
    if (esp_ae_drc_set_makeup_gain(c->drc, (float)gain_db) != ESP_AE_ERR_OK) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t _open(audio_element_handle_t self)
{
    lim_ctx_t *c = ctx_of(self);

    c->buf = (int16_t *)audio_calloc(1, LIM_BUF_BYTES);
    if (!c->buf) return ESP_ERR_NO_MEM;

    int8_t g = clamp_gain(c->gain_req);
    esp_ae_drc_curve_point pts[3];
    uint8_t npts = curve_for(g, pts);
    esp_ae_drc_cfg_t cfg = {
        .sample_rate     = (uint32_t)c->samplerate,
        .channel         = (uint8_t)c->channels,
        .bits_per_sample = 16,
        .drc_para = {
            .point       = pts,
            .point_num   = npts,
            .makeup_gain = (float)g,
            .knee_width  = 0.0f,
            .attack_time = LIM_ATTACK_MS,
            .release_time = LIM_RELEASE_MS,
            .hold_time   = 0,
        },
    };
    if (esp_ae_drc_open(&cfg, &c->drc) != ESP_AE_ERR_OK || !c->drc) {
        ESP_LOGE(TAG, "esp_ae_drc_open failed");
        audio_free(c->buf);
        c->buf = NULL;
        c->drc = NULL;
        return ESP_FAIL;
    }
    c->gain_applied = g;
    return ESP_OK;
}

static esp_err_t _close(audio_element_handle_t self)
{
    lim_ctx_t *c = ctx_of(self);
    if (c->drc) { esp_ae_drc_close(c->drc); c->drc = NULL; }
    if (c->buf) { audio_free(c->buf);       c->buf = NULL; }
    return ESP_OK;
}

static audio_element_err_t _process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    lim_ctx_t *c = ctx_of(self);

    int r = audio_element_input(self, (char *)c->buf, LIM_BUF_BYTES);
    if (r <= 0) return (audio_element_err_t)r;

    int8_t want = clamp_gain(c->gain_req);
    if (want != c->gain_applied) {
        if (want != 0) {
            if (apply_gain(c, want) == ESP_OK) {
                /* leaving bypass: drop stale envelope state */
                if (c->gain_applied == 0) esp_ae_drc_reset(c->drc);
                c->gain_applied = want;
            } else {
                ESP_LOGW(TAG, "gain update to %d dB failed", (int)want);
            }
        } else {
            c->gain_applied = 0;
        }
    }

    if (c->gain_applied != 0) {
        uint32_t frames = (uint32_t)r / (uint32_t)(c->channels * (int)sizeof(int16_t));
        if (frames > 0u) {
            esp_ae_drc_process(c->drc, frames, (esp_ae_sample_t)c->buf, (esp_ae_sample_t)c->buf);
        }
    }

    return (audio_element_err_t)audio_element_output(self, (char *)c->buf, r);
}

static esp_err_t _destroy(audio_element_handle_t self)
{
    lim_ctx_t *c = ctx_of(self);
    if (c) audio_free(c);
    return ESP_OK;
}

esp_err_t limiter_el_set_gain_db(audio_element_handle_t self, int8_t gain_db)
{
    if (!self) return ESP_ERR_INVALID_ARG;
    lim_ctx_t *c = ctx_of(self);
    if (!c) return ESP_ERR_INVALID_STATE;
    c->gain_req = clamp_gain(gain_db);
    return ESP_OK;
}

audio_element_handle_t limiter_el_init(const limiter_el_cfg_t *cfg)
{
    if (!cfg) return NULL;

    lim_ctx_t *c = (lim_ctx_t *)audio_calloc(1, sizeof(lim_ctx_t));
    AUDIO_MEM_CHECK(TAG, c, return NULL);
    c->samplerate = cfg->samplerate;
    c->channels   = cfg->channels;

    audio_element_cfg_t el_cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    el_cfg.open         = _open;
    el_cfg.close        = _close;
    el_cfg.process      = _process;
    el_cfg.destroy      = _destroy;
    el_cfg.task_stack   = cfg->task_stack;
    el_cfg.task_prio    = cfg->task_prio;
    el_cfg.task_core    = cfg->task_core;
    el_cfg.out_rb_size  = cfg->out_rb_size;
    el_cfg.stack_in_ext = cfg->stack_in_ext;
    el_cfg.tag          = "limiter";

    audio_element_handle_t el = audio_element_init(&el_cfg);
    AUDIO_MEM_CHECK(TAG, el, { audio_free(c); return NULL; });
    audio_element_setdata(el, c);
    return el;
}
