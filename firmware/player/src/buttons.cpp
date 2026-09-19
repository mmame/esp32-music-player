#include "buttons.h"

#include "encoder.h"
#include "pins.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG      = "buttons";
static const char *MAP_PATH = "/sdcard/button_map.json";

#define BTN_MATCH_TOL      110   /* |raw - learned| counts that still count as "this button"   */
#define BTN_STABLE_TOL      60   /* max spread between samples of one press while learning     */
#define BTN_LEARN_SAMPLES   15   /* ~150 ms of stable samples are averaged                     */
#define BTN_LEARN_TIMEOUT_US (30LL * 1000000LL)

static const button_fn_info_t k_fns[BTN_FN_COUNT] = {
    { "player_playpause",  "Play / Pause",                    BTN_CTX_PLAYER },
    { "player_next",       "Next song",                       BTN_CTX_PLAYER },
    { "player_prev",       "Previous song",                   BTN_CTX_PLAYER },
    { "player_stop",       "Stop",                            BTN_CTX_PLAYER },
    { "player_end_action", "End of song: Stop / Next / Repeat", BTN_CTX_PLAYER },
    { "list_up",           "Up",                              BTN_CTX_LIST   },
    { "list_down",         "Down",                            BTN_CTX_LIST   },
    { "list_select",       "Play selected song",              BTN_CTX_LIST   },
};

static const uint16_t k_thresholds[BTN_COUNT] = BTN_THRESHOLDS;
#define BTN_IDLE_RAW  ((int)k_thresholds[BTN_COUNT - 1])   /* at/above the top rung = no press */

static int16_t           s_adc[BTN_FN_COUNT];   /* learned raw value per function, -1 = none */
static SemaphoreHandle_t s_mu = nullptr;

/* learning state */
static volatile btn_learn_state_t s_learn_state = BTN_LEARN_IDLE;
static int      s_learn_fn      = -1;
static int      s_learn_adc     = -1;
static char     s_learn_err[80] = "";
static int64_t  s_learn_start_us = 0;
static bool     s_wait_release  = false;
static int      s_acc_sum       = 0;
static int      s_acc_n         = 0;

/* latest physical press, shown live in the web UI */
static volatile uint32_t s_press_seq = 0;
static volatile int      s_press_fn  = -1;
static volatile int      s_press_adc = -1;
static volatile int64_t  s_press_us  = 0;

static inline void lock(void)   { if (s_mu) xSemaphoreTake(s_mu, portMAX_DELAY); }
static inline void unlock(void) { if (s_mu) xSemaphoreGive(s_mu); }

/* ── persistence ─────────────────────────────────────────────────────────── */

static void save_map(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    lock();
    for (int i = 0; i < BTN_FN_COUNT; i++) {
        if (s_adc[i] >= 0) cJSON_AddNumberToObject(root, k_fns[i].id, s_adc[i]);
    }
    unlock();
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return;

    FILE *f = fopen(MAP_PATH, "w");
    if (f) {
        fputs(str, f);
        fclose(f);
        ESP_LOGI(TAG, "Button map saved: %s", str);
    } else {
        ESP_LOGE(TAG, "Cannot write %s", MAP_PATH);
    }
    cJSON_free(str);
}

void buttons_load(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    for (int i = 0; i < BTN_FN_COUNT; i++) s_adc[i] = -1;

    struct stat st = {};
    if (stat(MAP_PATH, &st) != 0 || st.st_size <= 0 || st.st_size > 2048) return;

    FILE *f = fopen(MAP_PATH, "r");
    if (!f) return;
    char *buf = (char *)malloc((size_t)st.st_size + 1u);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1u, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Parse error in %s – no buttons assigned", MAP_PATH);
        return;
    }
    lock();
    for (int i = 0; i < BTN_FN_COUNT; i++) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, k_fns[i].id);
        if (cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 4095) {
            s_adc[i] = (int16_t)it->valueint;
        }
    }
    unlock();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Button map loaded");
}

/* ── queries ─────────────────────────────────────────────────────────────── */

const button_fn_info_t *buttons_fn_info(int fn)
{
    return (fn >= 0 && fn < BTN_FN_COUNT) ? &k_fns[fn] : nullptr;
}

int buttons_fn_by_id(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < BTN_FN_COUNT; i++) {
        if (strcmp(k_fns[i].id, id) == 0) return i;
    }
    return -1;
}

int buttons_get_adc(int fn)
{
    if (fn < 0 || fn >= BTN_FN_COUNT) return -1;
    lock();
    int v = s_adc[fn];
    unlock();
    return v;
}

int buttons_match(int raw_avg, btn_ctx_t ctx)
{
    if (raw_avg < 0) return -1;
    int best = -1, best_d = BTN_MATCH_TOL + 1;
    lock();
    for (int i = 0; i < BTN_FN_COUNT; i++) {
        if (k_fns[i].ctx != ctx || s_adc[i] < 0) continue;
        int d = abs(raw_avg - (int)s_adc[i]);
        if (d < best_d) { best_d = d; best = i; }
    }
    unlock();
    return best;
}

esp_err_t buttons_clear(int fn)
{
    if (fn >= BTN_FN_COUNT) return ESP_ERR_INVALID_ARG;
    lock();
    if (fn < 0) { for (int i = 0; i < BTN_FN_COUNT; i++) s_adc[i] = -1; }
    else        { s_adc[fn] = -1; }
    unlock();
    save_map();
    return ESP_OK;
}

void buttons_note_press(int raw_avg, int fn)
{
    s_press_fn  = fn;
    s_press_adc = raw_avg;
    s_press_us  = esp_timer_get_time();
    s_press_seq = s_press_seq + 1u;   /* written last: readers see a complete record */
}

void buttons_last_press_get(uint32_t *seq, int *fn, int *adc, uint32_t *age_ms)
{
    uint32_t s = s_press_seq;
    if (seq)    *seq    = s;
    if (fn)     *fn     = s_press_fn;
    if (adc)    *adc    = s_press_adc;
    if (age_ms) *age_ms = (s == 0u) ? 0u : (uint32_t)((esp_timer_get_time() - s_press_us) / 1000);
}

/* ── learning ────────────────────────────────────────────────────────────── */

esp_err_t buttons_learn_start(int fn)
{
    if (fn < 0 || fn >= BTN_FN_COUNT) return ESP_ERR_INVALID_ARG;
    lock();
    s_learn_fn       = fn;
    s_learn_adc      = -1;
    s_learn_err[0]   = '\0';
    s_learn_start_us = esp_timer_get_time();
    s_wait_release   = true;   /* a button that is already held must be released first */
    s_acc_sum        = 0;
    s_acc_n          = 0;
    s_learn_state    = BTN_LEARN_WAITING;
    unlock();
    ESP_LOGI(TAG, "Learning '%s': waiting for a button press", k_fns[fn].id);
    return ESP_OK;
}

void buttons_learn_cancel(void)
{
    lock();
    if (s_learn_state == BTN_LEARN_WAITING) {
        s_learn_state = BTN_LEARN_IDLE;
        ESP_LOGI(TAG, "Learning cancelled");
    }
    unlock();
}

bool buttons_is_learning(void)
{
    return s_learn_state == BTN_LEARN_WAITING;
}

static void learn_fail(const char *msg)
{
    lock();
    snprintf(s_learn_err, sizeof(s_learn_err), "%s", msg);
    s_learn_state = BTN_LEARN_ERROR;
    unlock();
    ESP_LOGW(TAG, "Learning failed: %s", msg);
}

void buttons_learn_poll(void)
{
    if (s_learn_state != BTN_LEARN_WAITING) return;

    if (esp_timer_get_time() - s_learn_start_us > BTN_LEARN_TIMEOUT_US) {
        learn_fail("Timed out - no button was pressed.");
        return;
    }

    int raw = encoder_btn_read_raw();
    bool pressed = (raw >= 0 && raw < BTN_IDLE_RAW);
    if (!pressed) {
        s_wait_release = false;
        s_acc_sum = 0;
        s_acc_n   = 0;
        return;
    }
    if (s_wait_release) return;

    if (s_acc_n > 0 && abs(raw - s_acc_sum / s_acc_n) > BTN_STABLE_TOL) {
        s_acc_sum = raw;     /* unstable (contact bounce / another rung): start over */
        s_acc_n   = 1;
        return;
    }
    s_acc_sum += raw;
    s_acc_n++;
    if (s_acc_n < BTN_LEARN_SAMPLES) return;

    const int avg = s_acc_sum / s_acc_n;
    const int fn  = s_learn_fn;

    /* One button can only have one function per screen context. */
    int conflict = -1;
    lock();
    for (int i = 0; i < BTN_FN_COUNT; i++) {
        if (i == fn || k_fns[i].ctx != k_fns[fn].ctx || s_adc[i] < 0) continue;
        if (abs(avg - (int)s_adc[i]) <= BTN_MATCH_TOL) { conflict = i; break; }
    }
    unlock();

    if (conflict >= 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "This button is already assigned to \"%s\" on the same screen.",
                 k_fns[conflict].label);
        learn_fail(msg);
        return;
    }

    lock();
    s_adc[fn]     = (int16_t)avg;
    s_learn_adc   = avg;
    s_learn_state = BTN_LEARN_DONE;
    unlock();
    ESP_LOGI(TAG, "Learned '%s' = ADC %d", k_fns[fn].id, avg);
    save_map();
}

void buttons_learn_get(button_learn_status_t *out)
{
    if (!out) return;
    lock();
    out->state = s_learn_state;
    out->fn    = s_learn_fn;
    out->adc   = s_learn_adc;
    snprintf(out->error, sizeof(out->error), "%s", s_learn_err);
    unlock();
}
