#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "audio_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_GRAINS        16
#define GRAIN_HANN_LUT_SZ 512
#define GRAIN_BUF_SAMPLES 44100   // 1 second of samples per buffer

typedef struct {
    float pos;
    float pitch;
    float age;
    float lifetime;
    bool  active;
} grain_t;

typedef struct {
    float * volatile buf_L[2];
    float * volatile buf_R[2];
    volatile int      active_buf;
    volatile bool     loading;
    volatile uint32_t buf_samples;
    uint32_t          loaded_offset;
    grain_t           grains[MAX_GRAINS];
    float position;
    float spray;
    float size_ms;
    float density;
    float pitch;
    float pitch_spray;
    float timer;
    float hann_lut[GRAIN_HANN_LUT_SZ];
} granular_t;

void granular_init(granular_t *g);
void granular_free(granular_t *g);
void granular_update_params(granular_t *g, float position, float spray,
                            float size_ms, float density,
                            float pitch_semitones, float pitch_spray);
bool granular_load_needed(granular_t *g, uint32_t fsize);
void granular_load_buffer(granular_t *g, audio_f_t *file, SemaphoreHandle_t *mutex);
void granular_process_sample(granular_t *g, float *L_out, float *R_out);
