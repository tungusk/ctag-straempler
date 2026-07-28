// M4 granular engine — grain-cloud playback of a mono PSRAM sample.
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sample_ram.h"
#include "machine.h"
#include "cvsmooth.h"
#include "audio.h"
#include "granular_priv.h"

gr_state_t gr;

static float s_hann[257];   // raised-cosine window LUT (0..256, +1 guard)

static inline float rng_uni(void)   // 0..1
{
    gr.rng = gr.rng * 1664525u + 1013904223u;
    return (float)(gr.rng >> 8) / (float)(1u << 24);
}

// CUBIC read, matching Keys' cubic_read. Linear interpolation is dull and
// aliased on pitch-shifted reads and gets worse the further `inc` moves from
// 1.0 — i.e. exactly when the cloud is played chromatically, which is where
// this machine is heading (tuned texture, V/oct from a window).
static inline float gr_cubic(const int16_t *b, uint32_t n, double pos)
{
    long i1 = (long)pos;
    float t = (float)(pos - (double)i1);
    long i0 = i1 - 1, i2 = i1 + 1, i3 = i1 + 2;
    if (i0 < 0) i0 = 0;
    if (i1 >= (long)n) i1 = (long)n - 1;
    if (i2 >= (long)n) i2 = (long)n - 1;
    if (i3 >= (long)n) i3 = (long)n - 1;
    float y0 = b[i0], y1 = b[i1], y2 = b[i2], y3 = b[i3];
    float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    float a1 =        y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float a2 = -0.5f * y0            + 0.5f * y2;
    return ((a0 * t + a1) * t + a2) * t + y1;
}

static void spawn_grain(void)
{
    grain_t *g = NULL;
    for (int i = 0; i < GR_GRAINS; i++) if (!gr.grains[i].active) { g = &gr.grains[i]; break; }
    // POOL FULL: steal the grain nearest the end of its window rather than
    // dropping this one. Overlap is density x grain length, so 40 grains/s at
    // 500 ms wants 20 voices against a pool of 16 — past that the old code
    // silently punched holes in the cloud, which is a large part of why it did
    // not sound smooth. Stealing the most-finished grain costs the tail of a
    // grain already fading under its own window, which is the least audible
    // thing available.
    if (!g) {
        float worst = -1.0f;
        for (int i = 0; i < GR_GRAINS; i++) {
            if (gr.grains[i].wphase > worst) { worst = gr.grains[i].wphase; g = &gr.grains[i]; }
        }
        if (!g) return;
    }

    uint32_t glen = (uint32_t)gr.grain_ms * GR_RATE / 1000;
    if (glen < 32) glen = 32;
    if (glen > gr.len) glen = gr.len;

    // pitch increment (unity plateau around centre like the slicer)
    float inc;
    if (gr.pitch_cv >= 1843 && gr.pitch_cv <= 2253) inc = 1.0f;
    else if (gr.pitch_cv > 2253) inc = 1.0f + (float)(gr.pitch_cv - 2253) / 1842.0f;
    else                         inc = 0.5f + (float)gr.pitch_cv / 1843.0f * 0.5f;

    // position + spray jitter (spray scales up to ~0.25 s of wander)
    double jitter = (double)((rng_uni() * 2.0f - 1.0f) * gr.spray / 100.0f) * (GR_RATE / 4);
    double p = gr.base_pos + jitter;
    double maxp = (double)gr.len - (double)glen * inc - 2.0;
    if (maxp < 0) maxp = 0;
    if (p < 0) p = 0;
    if (p > maxp) p = maxp;

    // per-grain pan for stereo spread
    float pan = 0.5f + (rng_uni() * 2.0f - 1.0f) * ((float)gr.spread / 100.0f) * 0.5f;
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;

    g->pos = p;
    g->inc = inc;
    g->wphase = 0.0f;
    g->wstep = 256.0f / (float)glen;
    g->panL = 1.0f - pan;
    g->panR = pan;
    g->active = true;
}

// ---- lifecycle ------------------------------------------------------------
static esp_err_t granular_start(void)
{
    memset(&gr, 0, sizeof(gr));
    gr.buf = heap_caps_malloc((size_t)GR_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!gr.buf) { ESP_LOGE("GRAN", "PSRAM alloc failed"); return ESP_ERR_NO_MEM; }
    gr.position = 0;
    gr.pitch_cv = 2048;
    gr.grain_ms = 120;
    gr.density  = 20;
    gr.spray    = 20;
    gr.spread   = 50;
    gr.level    = 255;
    gr.rng      = 0x1234567u;
    for (int i = 0; i <= 256; i++) s_hann[i] = 0.5f * (1.0f - cosf((float)M_PI * 2.0f * i / 256.0f));

    char first[1][24];
    if (granular_list_samples(first, 1) > 0) granular_load(first[0]);
    audio_status_set_voices("granular", "");
    return ESP_OK;
}

static void granular_stop(void)
{
    free(gr.buf);
    gr.buf = NULL;
}

// ---- audio ---------------------------------------------------------------
static void granular_process(int32_t out[MACHINE_BLOCK],
                             const int32_t in[MACHINE_BLOCK],
                             const machine_io_t *io)
{
    // conditioned CV (cvsmooth.h): the ADC throws lone outliers (a channel sits at a
    // steady ~1221 and reports ONE sample of 4). CV1 drives a GAIN here, so a spike is
    // a click — and note the >900 floor-gate turns a DOWN-spike into c1 = 0, which maps
    // to UNITY, i.e. a jump to full level. The clock read stays RAW (clockin has its own
    // Schmitt and needs the edge timing).
    static cvmed_t s_med[8];
    int cvm[8];
    for (int k = 0; k < 8; k++) cvm[k] = cvmed_step(&s_med[k], io->cv[k]);

    (void)in;
    if (gr.loading || gr.len == 0 || !gr.buf) { memset(out, 0, MACHINE_BLOCK * sizeof(int32_t)); return; }

    // TR1 held (active low) = freeze the cloud position
    gr.freeze = !(io->trig_level & 1);

    gr.position = cvm[5];     // knob 6 = position
    gr.pitch_cv = cvm[6];     // knob 7 = pitch
    uint16_t c1 = cvm[0] > 900 ? cvm[0] - 900 : 0;   // CV1 jack = level
    gr.level = c1 ? (uint16_t)((uint32_t)c1 * 255 / 3195) : 255;

    if (!gr.freeze)
        gr.base_pos = (double)gr.position / 4095.0 * (double)(gr.len > 1 ? gr.len - 1 : 0);

    float spawn_per_sample = (float)gr.density / (float)GR_RATE;
    int frames = MACHINE_BLOCK / 2;
    int active = 0;

    for (int f = 0; f < frames; f++) {
        gr.spawn_phase += spawn_per_sample;
        while (gr.spawn_phase >= 1.0f) { gr.spawn_phase -= 1.0f; spawn_grain(); }

        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < GR_GRAINS; i++) {
            grain_t *g = &gr.grains[i];
            if (!g->active) continue;
            float s = gr_cubic(gr.buf, gr.len, g->pos);
            float win = s_hann[(int)g->wphase & 0xFF];
            s *= win;
            l += s * g->panL;
            r += s * g->panR;
            g->pos += g->inc;
            g->wphase += g->wstep;
            if (g->wphase >= 256.0f || g->pos >= (double)gr.len) g->active = false;
        }

        int32_t li = (int32_t)(l * gr.level) >> 8;
        int32_t ri = (int32_t)(r * gr.level) >> 8;
        if (li > 32767) li = 32767; else if (li < -32768) li = -32768;
        if (ri > 32767) ri = 32767; else if (ri < -32768) ri = -32768;
        out[f * 2]     = li << 16;
        out[f * 2 + 1] = ri << 16;
    }
    for (int i = 0; i < GR_GRAINS; i++) if (gr.grains[i].active) active++;
    gr.active_count = active;
}

// ---- sample I/O (UI task) -------------------------------------------------
static void compute_peaks(void)
{
    for (int c = 0; c < GR_PEAKS; c++) {
        uint32_t a = (uint32_t)((uint64_t)c * gr.len / GR_PEAKS);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * gr.len / GR_PEAKS);
        int peak = 0;
        for (uint32_t i = a; i < b; i++) { int v = gr.buf[i]; if (v < 0) v = -v; if (v > peak) peak = v; }
        gr.peaks[c] = (uint8_t)((peak * 31) / 32768);
    }
    gr.peak_n = (gr.len > 0) ? GR_PEAKS : 0;
}

int granular_load(const char *name)
{
    if (!gr.buf) return -1;
    gr.loading = true;
    for (int i = 0; i < GR_GRAINS; i++) gr.grains[i].active = false;
    uint32_t n = sample_load(name, gr.buf, GR_MAX_FRAMES, true);   // mono
    if (n == 0) { gr.loading = false; return -1; }
    gr.len = n;
    strncpy(gr.sample, name, sizeof(gr.sample) - 1);
    gr.sample[sizeof(gr.sample) - 1] = 0;
    compute_peaks();
    gr.loading = false;
    ESP_LOGI("GRAN", "loaded %s (%lu samples)", name, (unsigned long)n);
    return 0;
}

int granular_list_samples(char out[][24], int max)
{
    return sample_list(out, max);
}

// ---- preset ---------------------------------------------------------------
static cJSON *granular_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "grain_ms", gr.grain_ms);
    cJSON_AddNumberToObject(o, "density", gr.density);
    cJSON_AddNumberToObject(o, "spray", gr.spray);
    cJSON_AddNumberToObject(o, "spread", gr.spread);
    cJSON_AddStringToObject(o, "sample", gr.sample);
    return o;
}

static void granular_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "grain_ms")) && cJSON_IsNumber(j)) gr.grain_ms = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "density")) && cJSON_IsNumber(j))  gr.density = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "spray")) && cJSON_IsNumber(j))    gr.spray = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "spread")) && cJSON_IsNumber(j))   gr.spread = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sample")) && cJSON_IsString(j) && j->valuestring[0])
        granular_load(j->valuestring);
}

extern const machine_ui_t granular_menu_ui;

const machine_t machine_granular = {
    .name = "Granular",
    .start = granular_start,
    .stop = granular_stop,
    .process = granular_process,
    .preset_save = granular_preset_save,
    .preset_load = granular_preset_load,
    .ui = &granular_menu_ui,
};
