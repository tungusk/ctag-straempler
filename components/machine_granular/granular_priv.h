#pragma once
#include <stdint.h>
#include <stdbool.h>

// M4 granular — a grain cloud over a MONO PSRAM sample. A fixed pool of grains,
// each a raised-cosine-windowed, linear-interpolated read from a position in
// the sample, panned per-grain for stereo. Engine (audio task) owns the grain
// pool; the UI task writes params/commands and reads for display.

#define GR_RATE       44100
#define GR_MAX_SECS   12
#define GR_MAX_FRAMES (GR_RATE * GR_MAX_SECS)   // mono samples cap
#define GR_GRAINS     16                        // grain pool size
#define GR_PEAKS      300

typedef struct {
    bool   active;
    double pos;        // read position in the sample (mono samples)
    float  inc;        // pitch increment (samples per output sample)
    float  wphase;     // window phase 0..256 (indexes the Hann LUT)
    float  wstep;      // window advance per sample
    float  panL, panR;
} grain_t;

typedef struct {
    int16_t *buf;                 // MONO PSRAM, GR_MAX_FRAMES samples
    volatile uint32_t len;        // samples loaded
    volatile bool loading;
    char sample[24];

    // params (UI writes, engine reads)
    volatile int  position;       // 0..4095 cloud position (CV6)
    volatile int  pitch_cv;       // 0..4095 (CV7)
    volatile int  grain_ms;       // 10..500 grain length
    volatile int  density;        // grains/sec
    volatile int  spray;          // 0..100 position jitter %
    volatile int  spread;         // 0..100 pan spread %
    volatile int  level;          // 0..255 (CV1 jack)
    volatile bool freeze;         // hold the cloud position

    // engine state
    grain_t  grains[GR_GRAINS];
    float    spawn_phase;
    uint32_t rng;
    double   base_pos;            // current cloud read position (samples)
    volatile int active_count;    // grains currently sounding (display)

    // waveform display
    volatile int peak_n;
    uint8_t peaks[GR_PEAKS];
} gr_state_t;

extern gr_state_t gr;

int  granular_load(const char *name);
int  granular_list_samples(char out[][24], int max);
