// Shared flanger — see flanger.h.
//
// A raised-cosine LFO sweeps a fractional delay between ~1 ms and (depth)-scaled
// ~11 ms; the swept tap is fed back and mixed with the dry signal equal-power.
// Interpolated read makes the sweep smooth (an integer tap would zipper).

#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "flanger.h"

static const char *TAG = "FLANGER";

esp_err_t flanger_init(flanger_t *g)
{
    memset(g, 0, sizeof(*g));
    g->bufL = heap_caps_calloc((size_t)FLG_MAX_FR * 2, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!g->bufL) {
        ESP_LOGE(TAG, "PSRAM slab alloc failed (%u B)",
                 (unsigned)((size_t)FLG_MAX_FR * 2 * sizeof(float)));
        return ESP_ERR_NO_MEM;
    }
    g->cap  = FLG_MAX_FR;
    g->bufR = g->bufL + g->cap;
    g->rate  = 0.3f;
    g->depth = 0.6f;
    g->fb    = 0.25f;        // gentle default: high feedback rings up on held tones
    g->wet   = 0.0f;
    return ESP_OK;
}

void flanger_free(flanger_t *g)
{
    if (g->bufL) heap_caps_free(g->bufL);
    g->bufL = g->bufR = NULL;
    g->cap = 0;
}

void flanger_clear(flanger_t *g)
{
    if (g->bufL) memset(g->bufL, 0, (size_t)g->cap * 2 * sizeof(float));
    g->lpL = g->lpR = 0.0f;
}

void flanger_block_i32(flanger_t *g, int32_t *out, int frames)
{
    if (!g->bufL) return;
    const int cap = g->cap;
    float rate  = g->rate;  if (rate < 0.01f) rate = 0.01f; else if (rate > 10.0f) rate = 10.0f;
    float depth = g->depth; if (depth < 0) depth = 0; else if (depth > 1) depth = 1;
    float fb    = g->fb;    if (fb < -0.95f) fb = -0.95f; else if (fb > 0.95f) fb = 0.95f;
    float wet   = g->wet;   if (wet < 0) wet = 0; else if (wet > 1) wet = 1;
    float wetg  = sinf(wet * 1.5707963f);
    float dryg  = cosf(wet * 1.5707963f);

    const float mind  = 1.0f * FLG_RATE / 1000.0f;   // 1 ms floor
    const float sweep = depth * ((float)(cap - 2) - mind);
    const float inc   = rate / (float)FLG_RATE;
    const float dc    = 0.5f;                         // feedback damping (LP the ring)
    float ph  = g->phase;
    float lpL = g->lpL, lpR = g->lpR;
    int   w   = g->w;

    for (int f = 0; f < frames; f++) {
        float lfo = 0.5f - 0.5f * cosf(6.2831853f * ph);   // 0..1
        float dl  = mind + sweep * lfo;                    // fractional delay (frames)
        float rp  = (float)w - dl; while (rp < 0) rp += cap;
        int   i0  = (int)rp; float fr = rp - (float)i0;
        int   i1  = i0 + 1; if (i1 >= cap) i1 -= cap;
        float tl  = g->bufL[i0] * (1.0f - fr) + g->bufL[i1] * fr;
        float tr  = g->bufR[i0] * (1.0f - fr) + g->bufR[i1] * fr;
        float xl  = (float)(out[f * 2]     >> 16);
        float xr  = (float)(out[f * 2 + 1] >> 16);
        // damp the RECIRCULATED signal so a swept comb + feedback can't ring up
        // into metallic harshness on a sustained tone (the wet tap stays bright)
        lpL += dc * (tl - lpL);
        lpR += dc * (tr - lpR);
        float wl = xl + fb * lpL, wr = xr + fb * lpR;
        if (wl > 60000.0f) wl = 60000.0f; else if (wl < -60000.0f) wl = -60000.0f;
        if (wr > 60000.0f) wr = 60000.0f; else if (wr < -60000.0f) wr = -60000.0f;
        g->bufL[w] = wl;
        g->bufR[w] = wr;
        float ol = xl * dryg + tl * wetg;
        float or_ = xr * dryg + tr * wetg;
        int32_t l = (int32_t)ol, r = (int32_t)or_;
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[f * 2]     = l << 16;
        out[f * 2 + 1] = r << 16;
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        if (++w >= cap) w = 0;
    }
    g->phase = ph;
    g->w = w;
    // NaN guard: a runaway feedback path must never latch permanent noise
    if (!(fabsf(lpL) < 1e9f) || !(fabsf(lpR) < 1e9f)) { lpL = lpR = 0.0f; flanger_clear(g); }
    g->lpL = lpL; g->lpR = lpR;
}
