#include "granular.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "GRAN";

void granular_init(granular_t *g) {
    memset(g, 0, sizeof(granular_t));
    for (int b = 0; b < 2; b++) {
        g->buf_L[b] = (float *)heap_caps_malloc(GRAIN_BUF_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g->buf_R[b] = (float *)heap_caps_malloc(GRAIN_BUF_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g->buf_L[b] || !g->buf_R[b])
            ESP_LOGE(TAG, "Failed to alloc grain buffer %d", b);
    }
    for (int i = 0; i < GRAIN_HANN_LUT_SZ; i++)
        g->hann_lut[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)(GRAIN_HANN_LUT_SZ - 1)));
    g->position     = 0.5f;
    g->spray        = 0.1f;
    g->size_ms      = 80.0f;
    g->density      = 12.0f;
    g->pitch        = 1.0f;
    g->pitch_spray  = 0.0f;
    g->timer        = 0.0f;
    g->active_buf   = 0;
    g->loading      = false;
    g->buf_samples  = 0;
    g->loaded_offset = UINT32_MAX;
}

void granular_free(granular_t *g) {
    for (int b = 0; b < 2; b++) {
        if (g->buf_L[b]) { heap_caps_free(g->buf_L[b]); g->buf_L[b] = NULL; }
        if (g->buf_R[b]) { heap_caps_free(g->buf_R[b]); g->buf_R[b] = NULL; }
    }
}

void granular_update_params(granular_t *g, float position, float spray,
                            float size_ms, float density,
                            float pitch_semitones, float pitch_spray) {
    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;
    g->position    = position;
    g->spray       = spray < 0.0f ? 0.0f : (spray > 1.0f ? 1.0f : spray);
    g->size_ms     = size_ms < 5.0f ? 5.0f : (size_ms > 500.0f ? 500.0f : size_ms);
    g->density     = density < 1.0f ? 1.0f : (density > 50.0f ? 50.0f : density);
    g->pitch       = powf(2.0f, pitch_semitones / 12.0f);
    g->pitch_spray = pitch_spray < 0.0f ? 0.0f : (pitch_spray > 1.0f ? 1.0f : pitch_spray);
}

bool granular_load_needed(granular_t *g, uint32_t fsize) {
    if (g->loading) return false;
    if (g->buf_samples == 0) return true;
    if (fsize < 8) return false;
    uint32_t max_offset = fsize > (GRAIN_BUF_SAMPLES * 4u) ? fsize - (GRAIN_BUF_SAMPLES * 4u) : 0;
    uint32_t target = (uint32_t)(g->position * (float)max_offset);
    uint32_t loaded = g->loaded_offset;
    uint32_t delta  = target > loaded ? target - loaded : loaded - target;
    return delta > (GRAIN_BUF_SAMPLES * 4u / 10u);
}

void granular_load_buffer(granular_t *g, audio_f_t *file, SemaphoreHandle_t *mutex) {
    if (!g->buf_L[0] || !g->buf_L[1]) return;
    g->loading = true;
    int write_buf = 1 - g->active_buf;
    uint32_t fsize = file->fsize;
    uint32_t max_offset = fsize > (GRAIN_BUF_SAMPLES * 4u) ? fsize - (GRAIN_BUF_SAMPLES * 4u) : 0;
    uint32_t offset = (uint32_t)(g->position * (float)max_offset);
    offset = (offset / 4u) * 4u;

    uint8_t chunk[512];
    UINT n;
    uint32_t samples = 0;

    xSemaphoreTake(*mutex, portMAX_DELAY);
    f_lseek(&file->fil, offset);
    while (samples < GRAIN_BUF_SAMPLES) {
        uint32_t want = (GRAIN_BUF_SAMPLES - samples) * 4u;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        f_read(&file->fil, chunk, want, &n);
        if (n == 0) break;
        for (uint32_t i = 0; i + 3 < n && samples < GRAIN_BUF_SAMPLES; i += 4) {
            int16_t l = (int16_t)((uint16_t)chunk[i]   | ((uint16_t)chunk[i + 1] << 8));
            int16_t r = (int16_t)((uint16_t)chunk[i + 2] | ((uint16_t)chunk[i + 3] << 8));
            g->buf_L[write_buf][samples] = (float)l * (1.0f / 32768.0f);
            g->buf_R[write_buf][samples] = (float)r * (1.0f / 32768.0f);
            samples++;
        }
    }
    xSemaphoreGive(*mutex);

    g->loaded_offset = offset;
    g->buf_samples   = samples;
    __sync_synchronize();       // buf_samples visible before active_buf swap
    g->active_buf    = write_buf;
    __sync_synchronize();
    g->loading = false;
}

static void spawn_grain(granular_t *g) {
    for (int i = 0; i < MAX_GRAINS; i++) {
        if (g->grains[i].active) continue;
        grain_t *gr = &g->grains[i];
        gr->active   = true;
        gr->age      = 0.0f;
        gr->lifetime = g->size_ms * 0.001f * 44100.0f;
        if (gr->lifetime < 1.0f) gr->lifetime = 1.0f;

        uint32_t n_samples  = g->buf_samples;
        float    center     = g->position * (float)n_samples;
        float    spray_half = g->spray * (float)n_samples * 0.5f;
        float    rand_f     = ((float)(esp_random() & 0xFFFFFF) / (float)0x1000000) * 2.0f - 1.0f;
        gr->pos = center + rand_f * spray_half;
        if (gr->pos < 0.0f) gr->pos = 0.0f;
        if (n_samples > 1 && gr->pos >= (float)(n_samples - 1)) gr->pos = (float)(n_samples - 2);

        float pitch_rand = ((float)(esp_random() & 0xFFFFFF) / (float)0x1000000) * 2.0f - 1.0f;
        gr->pitch = g->pitch * (1.0f + pitch_rand * g->pitch_spray * 0.1f);
        if (gr->pitch < 0.1f) gr->pitch = 0.1f;
        if (gr->pitch > 4.0f) gr->pitch = 4.0f;
        return;
    }
}

void granular_process_sample(granular_t *g, float *L_out, float *R_out) {
    g->timer -= 1.0f;
    if (g->timer <= 0.0f && g->buf_samples > 0) {
        spawn_grain(g);
        float d = g->density > 0.0f ? g->density : 1.0f;
        g->timer += 44100.0f / d;
    }

    float  sumL     = 0.0f, sumR = 0.0f;
    int    abuf     = g->active_buf;
    float *bufL     = g->buf_L[abuf];
    float *bufR     = g->buf_R[abuf];
    uint32_t buf_sz = g->buf_samples;

    if (!bufL || !bufR || buf_sz == 0) {
        *L_out = 0.0f;
        *R_out = 0.0f;
        return;
    }

    for (int i = 0; i < MAX_GRAINS; i++) {
        grain_t *gr = &g->grains[i];
        if (!gr->active) continue;

        int hann_idx = (int)(gr->age * (float)(GRAIN_HANN_LUT_SZ - 1));
        if (hann_idx < 0) hann_idx = 0;
        if (hann_idx >= GRAIN_HANN_LUT_SZ) hann_idx = GRAIN_HANN_LUT_SZ - 1;
        float w = g->hann_lut[hann_idx];

        int   pos_int = (int)gr->pos;
        float frac    = gr->pos - (float)pos_int;
        float sL, sR;
        if (pos_int >= 0 && (uint32_t)(pos_int + 1) < buf_sz) {
            sL = bufL[pos_int] + frac * (bufL[pos_int + 1] - bufL[pos_int]);
            sR = bufR[pos_int] + frac * (bufR[pos_int + 1] - bufR[pos_int]);
        } else if (pos_int >= 0 && (uint32_t)pos_int < buf_sz) {
            sL = bufL[pos_int];
            sR = bufR[pos_int];
        } else {
            gr->active = false;
            continue;
        }
        sumL += sL * w;
        sumR += sR * w;

        gr->pos += gr->pitch;
        gr->age += 1.0f / gr->lifetime;
        if (gr->age >= 1.0f || (uint32_t)gr->pos >= buf_sz)
            gr->active = false;
    }

    *L_out = sumL;
    *R_out = sumR;
}
