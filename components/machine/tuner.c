// Line-in chromatic tuner (see tuner.h). The audio task fills one of two
// 4096-frame mono buffers; when one fills it is handed to a prio-4 task that
// runs util/pitch_detect over it and publishes a smoothed reading. Two whole
// buffers rather than a wrapping ring because pitch_detect_window() wants a
// CONTIGUOUS window — with 93 ms per buffer and ~15 ms of detection, the
// analysis is always long finished before the audio task comes back round.
//
// Display steadiness follows the beatlisten lesson (no jumping around): the
// note only changes after two consecutive windows agree on it, the cents
// needle is slewed, and a few silent windows are needed before the readout
// drops out.
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "pitch_detect.h"
#include "tuner.h"

static const char *TAG = "TUNER";

#define TU_RATE     44100.0f
#define TU_WIN      4096        // frames per analysis buffer (~93 ms)
#define TU_MISSES   3           // silent windows before the readout drops out
#define TU_AGREE    2           // windows that must agree before the note changes
// One detection measured ~40 ms on hardware, and buffers arrive every 93 ms —
// analysing every one costs ~40% of a core and pushed the audio-loop meter from
// 517 to 592 us. A tuner needs about four readings a second, not eleven, so
// buffers arriving inside this window are dropped on the floor.
#define TU_MIN_GAP_US 250000
// Level gate. pitch_detect has its own silence floor, but it is set for OFFLINE
// use on a deliberately chosen sample; a tuner sits on a live Eurorack input
// where hum and bleed are always present, and WILL happily report a note at
// 30-60 Hz from noise. Below this it says "listening" instead of lying.
#define TU_RMS_MIN  400.0f      // int16 scale, ~ -38 dBFS
#define TU_CONF_MIN 0.50f       // stricter than the offline path, same reason

static int16_t *s_buf[2];               // the two analysis buffers (PSRAM)
static void    *s_scratch;              // pitch_detect scratch (allocated once)
static volatile int s_cur;              // buffer the audio task is filling
static volatile int s_fill;             // frames written into it
static volatile int s_ready = -1;       // full buffer waiting for the task
static volatile bool s_on;
static TaskHandle_t s_task;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static tuner_status_t s_st;             // published reading

bool tuner_get_enabled(void) { return s_on; }

void tuner_get_status(tuner_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    out->on = s_on;
}

// ---- audio task path -------------------------------------------------------

void tuner_push(const int32_t in[64])
{
    if (!s_on || !s_buf[0]) return;
    int16_t *b = s_buf[s_cur];
    int w = s_fill;
    for (int i = 0; i < 64; i += 2) {           // 32 stereo frames -> mono
        int32_t l = in[i] >> 16, r = in[i + 1] >> 16;
        b[w++] = (int16_t)((l + r) / 2);
    }
    if (w >= TU_WIN) {
        s_ready = s_cur;                        // hand this one to the task
        s_cur ^= 1;
        w = 0;
    }
    s_fill = w;
}

// ---- analysis task ---------------------------------------------------------

static void tu_task(void *arg)
{
    (void)arg;
    int pend_midi = -1, pend_n = 0, misses = 0;
    int64_t last_run = 0;
    for (;;) {
        if (!s_on || s_ready < 0) {
            vTaskDelay(1);                      // >=1 tick: never busy-spin
            continue;
        }
        int idx = s_ready;
        s_ready = -1;
        int64_t t0 = esp_timer_get_time();
        if (t0 - last_run < TU_MIN_GAP_US) continue;    // rate limit: drop this one
        last_run = t0;

        // level gate first — it is 1024 cheap reads versus ~40 ms of YIN, and
        // it is what stops noise from being reported as a note
        const int16_t *b = s_buf[idx];
        double e = 0.0;
        for (int i = 0; i < TU_WIN; i += 4) { double v = b[i]; e += v * v; }
        bool loud = sqrt(e / (TU_WIN / 4)) >= TU_RMS_MIN;

        pitch_result_t r;
        bool ok = loud &&
                  (pitch_detect_window(s_buf[idx], TU_WIN, TU_RATE, s_scratch, &r) == 0) &&
                  r.conf >= TU_CONF_MIN;
        int cost = (int)(esp_timer_get_time() - t0);

        portENTER_CRITICAL(&s_mux);
        s_st.cost_us = cost;
        portEXIT_CRITICAL(&s_mux);

        if (!ok) {
            if (++misses >= TU_MISSES) {
                pend_midi = -1; pend_n = 0;
                portENTER_CRITICAL(&s_mux);
                s_st.have = false;
                portEXIT_CRITICAL(&s_mux);
            }
            continue;
        }
        misses = 0;

        portENTER_CRITICAL(&s_mux);
        bool same = s_st.have && r.midi == s_st.midi;
        portEXIT_CRITICAL(&s_mux);

        if (same) {
            pend_midi = -1; pend_n = 0;
            portENTER_CRITICAL(&s_mux);         // same note: slew the needle
            s_st.cents = s_st.cents * 0.6f + r.cents * 0.4f;
            s_st.hz    = s_st.hz    * 0.6f + r.hz    * 0.4f;
            s_st.conf  = r.conf;
            portEXIT_CRITICAL(&s_mux);
            continue;
        }
        // a different note has to be seen twice running before it takes over —
        // one stray window mid-glide must not repaint the readout
        if (r.midi == pend_midi) pend_n++;
        else { pend_midi = r.midi; pend_n = 1; }
        if (pend_n >= TU_AGREE) {
            portENTER_CRITICAL(&s_mux);
            s_st.have  = true;
            s_st.midi  = r.midi;
            s_st.cents = r.cents;
            s_st.hz    = r.hz;
            s_st.conf  = r.conf;
            portEXIT_CRITICAL(&s_mux);
            pend_midi = -1; pend_n = 0;
        }
    }
}

// ---- enable ----------------------------------------------------------------

void tuner_set_enabled(bool on)
{
    if (on && !s_buf[0]) {
        // PSRAM for the buffers (the audio task only streams into them); the
        // detector scratch is small and hot, so it comes from internal
        s_buf[0] = heap_caps_malloc(TU_WIN * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_buf[1] = heap_caps_malloc(TU_WIN * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_scratch = malloc(pitch_scratch_bytes());
        if (!s_buf[0] || !s_buf[1] || !s_scratch) {
            ESP_LOGE(TAG, "alloc failed - staying OFF");
            if (s_buf[0])  { heap_caps_free(s_buf[0]);  s_buf[0] = NULL; }
            if (s_buf[1])  { heap_caps_free(s_buf[1]);  s_buf[1] = NULL; }
            if (s_scratch) { free(s_scratch); s_scratch = NULL; }
            return;
        }
    }
    if (on && !s_task) {
        // unpinned prio 4, the beatlisten/deck_analysis precedent. The task is
        // kept for the life of the boot and idles when off — tearing it down
        // under a buffer the audio task may still be filling is not worth it.
        if (xTaskCreate(tu_task, "tuner", 4096, NULL, 4, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "task create failed - staying OFF");
            s_task = NULL;
            return;
        }
    }
    if (on) {
        s_fill = 0; s_cur = 0; s_ready = -1;
        portENTER_CRITICAL(&s_mux);
        memset(&s_st, 0, sizeof(s_st));
        portEXIT_CRITICAL(&s_mux);
    }
    s_on = on;
}
