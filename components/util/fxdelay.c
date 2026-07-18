// Shared clock-syncable stereo DELAY — see fxdelay.h.
//
// One PSRAM slab holds two float lines (L, R). The kernel is a straight
// feedback delay: read the tap `len` frames back, low-pass it (damping), write
// dry + fb*tap at the head, mix the tap into the output equal-power. Ping-pong
// just swaps which line each feedback tap folds into. Everything is float end to
// end; the machine format (int32<<16) is converted at the block edges only.
//
// cost_us: EMA of process time, us per 64-frame block (1450 us = 100% of the
// audio tick) — same instrumentation contract as reverb, for a live cost meter.

#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "fxdelay.h"

static const char *TAG = "FXDELAY";

esp_err_t fxdelay_init(fxdelay_t *d)
{
    memset(d, 0, sizeof(*d));
    // one slab, two lines back to back (calloc = silence, no click on first read)
    d->bufL = heap_caps_calloc((size_t)FXD_MAX_FR * 2, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!d->bufL) {
        ESP_LOGE(TAG, "PSRAM slab alloc failed (%u KB)",
                 (unsigned)((size_t)FXD_MAX_FR * 2 * sizeof(float) / 1024));
        return ESP_ERR_NO_MEM;
    }
    d->cap  = FXD_MAX_FR;
    d->bufR = d->bufL + d->cap;
    d->w    = 0;
    d->len  = FXD_RATE * 3 / 8;      // 375 ms default (a musical starting point)
    d->fb   = 0.35f;
    d->wet  = 0.0f;                  // silent until a host opens the mix
    d->damp = 0.25f;
    ESP_LOGI(TAG, "slab %u KB PSRAM",
             (unsigned)((size_t)FXD_MAX_FR * 2 * sizeof(float) / 1024));
    return ESP_OK;
}

void fxdelay_free(fxdelay_t *d)
{
    if (d->bufL) heap_caps_free(d->bufL);
    d->bufL = d->bufR = NULL;
    d->cap = 0;
}

void fxdelay_clear(fxdelay_t *d)
{
    if (d->bufL) memset(d->bufL, 0, (size_t)d->cap * 2 * sizeof(float));
    d->lpL = d->lpR = 0.0f;
}

void fxdelay_set_time_ms(fxdelay_t *d, float ms)
{
    if (!d->cap) return;
    int len = (int)(ms * (float)FXD_RATE / 1000.0f + 0.5f);
    if (len < 1) len = 1;
    if (len > d->cap - 1) len = d->cap - 1;
    d->len = len;
}

void fxdelay_set_time_beats(fxdelay_t *d, float beats, float bpm)
{
    if (bpm < 1.0f) bpm = 1.0f;
    fxdelay_set_time_ms(d, beats * 60000.0f / bpm);
}

void fxdelay_set_feedback(fxdelay_t *d, float fb)
{
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 0.95f) fb = 0.95f;      // >0.95 runs away into self-oscillation
    d->fb = fb;
}

void fxdelay_set_mix(fxdelay_t *d, float wet)
{
    if (wet < 0.0f) wet = 0.0f;
    if (wet > 1.0f) wet = 1.0f;
    d->wet = wet;
}

void fxdelay_set_damp(fxdelay_t *d, float d01)
{
    if (d01 < 0.0f) d01 = 0.0f;
    if (d01 > 1.0f) d01 = 1.0f;
    d->damp = d01;
}

void fxdelay_set_pingpong(fxdelay_t *d, bool on) { d->pingpong = on; }

void fxdelay_block_i32(fxdelay_t *d, int32_t *out, int frames)
{
    if (!d->bufL) { d->cost_us = 0; return; }
    int64_t t0 = esp_timer_get_time();

    const int   cap = d->cap;
    const int   len = d->len < 1 ? 1 : (d->len > cap - 1 ? cap - 1 : d->len);
    const float fb  = d->fb;
    const float w01 = d->wet;
    const bool  pp  = d->pingpong;
    // equal-power wet/dry (matches reverb's crossfade feel)
    const float wetg = sinf(w01 * 1.5707963f);
    const float dryg = cosf(w01 * 1.5707963f);
    // damping one-pole coefficient: 1 = pass-through (bright), small = dark
    const float dc = 1.0f - 0.85f * d->damp;
    float lpL = d->lpL, lpR = d->lpR;
    int w = d->w;

    for (int f = 0; f < frames; f++) {
        int r = w - len; if (r < 0) r += cap;
        float dl = (float)(out[f * 2]     >> 16);
        float dr = (float)(out[f * 2 + 1] >> 16);
        float tl = d->bufL[r], tr = d->bufR[r];        // wet taps (pre-damp)
        // darken the signal that recirculates
        lpL += dc * (tl - lpL);
        lpR += dc * (tr - lpR);
        if (pp) {
            d->bufL[w] = dl + fb * lpR;
            d->bufR[w] = dr + fb * lpL;
        } else {
            d->bufL[w] = dl + fb * lpL;
            d->bufR[w] = dr + fb * lpR;
        }
        float ol  = dl * dryg + tl * wetg;
        float or_ = dr * dryg + tr * wetg;
        int32_t li = (int32_t)ol, ri = (int32_t)or_;
        if (li > 32767) li = 32767; else if (li < -32768) li = -32768;
        if (ri > 32767) ri = 32767; else if (ri < -32768) ri = -32768;
        out[f * 2]     = li << 16;
        out[f * 2 + 1] = ri << 16;
        if (++w >= cap) w = 0;
    }

    d->w = w;
    // NaN guard: a runaway feedback path must never latch permanent silence/noise
    if (!(fabsf(lpL) < 1e9f) || !(fabsf(lpR) < 1e9f)) { lpL = lpR = 0.0f; fxdelay_clear(d); }
    d->lpL = lpL; d->lpR = lpR;

    int us = (int)(esp_timer_get_time() - t0);
    d->cost_us += (us - d->cost_us) >> 3;              // EMA, ~8-block settle
}
