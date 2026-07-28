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
#include "fxchain.h"

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

void flanger_block_f(flanger_t *g, float *buf, int frames)
{
    if (!g->bufL) return;
    const int cap = g->cap;
    float rate  = g->rate;  if (rate < 0.01f) rate = 0.01f; else if (rate > 10.0f) rate = 10.0f;
    float depth = g->depth; if (depth < 0) depth = 0; else if (depth > 1) depth = 1;
    float fb    = g->fb;    if (fb < -0.95f) fb = -0.95f; else if (fb > 0.95f) fb = 0.95f;
    float wet   = g->wet;   if (wet < 0) wet = 0; else if (wet > 1) wet = 1;
    float wetg  = sinf(wet * 1.5707963f);
    float dryg  = cosf(wet * 1.5707963f);
    // FEEDBACK GAIN COMPENSATION (Arlo 2026-07-26: "overdriven sounding audio
    // when flanger is doubled with overdrive or reverb"). The recirculating ring
    // accumulates toward x/(1-fb) at the comb's resonant frequencies, and the wet
    // tap is read straight out of it — so the flanger handed the NEXT stage a
    // signal well above full scale. Measured NOT to be CPU (aus 678 of 1450) and
    // NOT output clipping (VU 98 of 255): it is gain staging inside the chain.
    // Trim the whole stage by the feedback amount so switching feedback up does
    // not also turn the level up into whatever follows.
    float outg  = 1.0f / (1.0f + fabsf(fb));

    const float mind  = 1.0f * FLG_RATE / 1000.0f;   // 1 ms floor
    const float sweep = depth * ((float)(cap - 2) - mind);
    const float inc   = rate / (float)FLG_RATE;
    const float dc    = 0.38f;                        // feedback damping (LP the ring;
                                                      // lower = darker recirculation,
                                                      // less metallic on held tones —
                                                      // the wet tap below stays bright)
    float ph  = g->phase;
    float lpL = g->lpL, lpR = g->lpR;
    int   w   = g->w;

    // LFO ONCE PER BLOCK, linearly interpolated across it — not cosf() per
    // sample. The LFO runs at 0.01-10 Hz, so over one 1.45 ms block it moves at
    // most ~1.5% of a cycle and a straight line through it is inaudible.
    //
    // This was 64 cosf() calls per block. Measured 2026-07-27 on the analog
    // output: flanger + a large reverb tank produced 1.33 discontinuities per
    // second of sound where flanger alone, hall alone, delay+hall and trem+hall
    // all produced ~0 — and the artefact survived mixing the flanger OUT,
    // zeroing its feedback and zeroing its sweep depth, so it was never the
    // signal. It is DMA underrun: deepening the I2S buffer to 17.4 ms cut it 91%
    // without touching the DSP. `aus`/`auspk`/`ausgap` all read normal
    // throughout, because i2s_write's portMAX_DELAY lets the loop catch up after
    // an underrun — no meter on the device can see this.
    float lfo0 = 0.5f - 0.5f * cosf(6.2831853f * ph);
    float ph_end = ph + inc * (float)frames;
    while (ph_end >= 1.0f) ph_end -= 1.0f;
    float lfo1 = 0.5f - 0.5f * cosf(6.2831853f * ph_end);
    float lfo_step = frames > 0 ? (lfo1 - lfo0) / (float)frames : 0.0f;
    float lfo = lfo0;

    for (int f = 0; f < frames; f++) {
        float dl  = mind + sweep * lfo;                    // fractional delay (frames)
        lfo += lfo_step;
        float rp  = (float)w - dl; while (rp < 0) rp += cap;
        int   i0  = (int)rp; float fr = rp - (float)i0;
        int   i1  = i0 + 1; if (i1 >= cap) i1 -= cap;
        float tl  = g->bufL[i0] * (1.0f - fr) + g->bufL[i1] * fr;
        float tr  = g->bufR[i0] * (1.0f - fr) + g->bufR[i1] * fr;
        float xl  = buf[f * 2];
        float xr  = buf[f * 2 + 1];
        // damp the RECIRCULATED signal so a swept comb + feedback can't ring up
        // into metallic harshness on a sustained tone (the wet tap stays bright)
        lpL += dc * (tl - lpL);
        lpR += dc * (tr - lpR);
        float wl = xl + fb * lpL, wr = xr + fb * lpR;
        // RUNAWAY GUARD ONLY — deliberately well above full scale. The ring holds
        // x + fb*feedback, which legitimately exceeds FS on ordinary material
        // (~1.33x at fb 0.25), so clamping it AT full scale hard-clips inside the
        // feedback loop and dirties everything downstream — tried 32767 on
        // 2026-07-26 and Arlo caught it on flanger+reverb at NORMAL level (output
        // VU only 100/255, so it was never an output-level problem). Level is the
        // job of outg above; this is purely a divergence backstop.
        if (wl > 60000.0f) wl = 60000.0f; else if (wl < -60000.0f) wl = -60000.0f;
        if (wr > 60000.0f) wr = 60000.0f; else if (wr < -60000.0f) wr = -60000.0f;
        g->bufL[w] = wl;
        g->bufR[w] = wr;
        buf[f * 2]     = (xl * dryg + tl * wetg) * outg;   // no clamp (chain soft-limits)
        buf[f * 2 + 1] = (xr * dryg + tr * wetg) * outg;
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        if (++w >= cap) w = 0;
    }
    g->phase = ph;
    g->w = w;
    // NaN guard: a runaway feedback path must never latch permanent noise
    if (!(fabsf(lpL) < 1e9f) || !(fabsf(lpR) < 1e9f)) { lpL = lpR = 0.0f; flanger_clear(g); }
    g->lpL = lpL; g->lpR = lpR;
}

// int32<<16 wrapper (single-FX callers): unpack -> worker -> hard-clamp pack.
void flanger_block_i32(flanger_t *g, int32_t *out, int frames)
{
    float buf[FX_SCRATCH_N];
    if (frames * 2 > FX_SCRATCH_N) frames = FX_SCRATCH_N / 2;
    fx_unpack_i32(out, buf, frames * 2);
    flanger_block_f(g, buf, frames);
    for (int i = 0; i < frames * 2; i++) {
        int32_t s = (int32_t)buf[i];
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        out[i] = s << 16;
    }
}
