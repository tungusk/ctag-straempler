#pragma once
#include <stdint.h>
#include <stdbool.h>

// M2 looper — shared state between engine (looper.c) and UI (looper_menu.c).
// Concurrency: engine (audio task) owns track state; UI writes only the
// volatile cmd_* flags and settings, reads everything else for display.

#define LP_TRACKS      4
#define LP_RATE        44100
#define LP_SECONDS     8
#define LP_BUF_FRAMES  (LP_RATE * LP_SECONDS)   // mono frames per track

// clock source values: 0..7 = CV1..CV8, 8 = TR1, 9 = TR2. A trig is the
// natural clock input (clean gate edge, no attenuverter knob in the path).
#define LP_CLK_TR1     8
#define LP_CLK_TR2     9
#define LP_CLK_SRCS    10

enum { LP_EMPTY = 0, LP_ARMED, LP_REC, LP_PLAY, LP_STOP };

typedef struct {
    int16_t *buf;                 // PSRAM, LP_BUF_FRAMES mono samples
    volatile uint32_t len;        // recorded length (frames)
    volatile uint32_t pos;        // play/record head
    volatile uint32_t target;     // record auto-stop point
    volatile uint8_t  state;
    volatile uint16_t vol;        // 0..255 level (Q8); driven by CV6 when selected
    volatile uint16_t pan;        // 0..4095, 2048 = center; driven by CV7 when selected
    volatile uint16_t cutoff;     // 0..4095 BP filter cutoff (CV1 when selected)
    volatile uint16_t res;        // 0..4095 BP resonance (CV2 when selected)
    float f, q;                   // per-block filter coeffs (engine)
    float svf_low, svf_band;      // state-variable filter state (engine)
} lp_track_t;

typedef struct {
    lp_track_t tr[LP_TRACKS];
    volatile int  sel;            // selected lane (UI + trig target)

    // settings (UI writes, engine reads)
    volatile bool sync_on;
    volatile int  clk_src;        // clock source (0..7 CV, 8 TR1, 9 TR2)
    volatile int  bars;           // loop length in bars when synced
    volatile bool monitor;        // pass line-in through to the output
    volatile bool filter_on;      // per-track bandpass filter enable

    // clock detector output (engine writes, UI reads)
    volatile float bpm;
    volatile bool  locked;

    // one-shot commands from UI (engine consumes)
    volatile uint8_t cmd_action[LP_TRACKS];  // context cycle: arm/cancel/punch/stop/play
    volatile uint8_t cmd_clear[LP_TRACKS];
} lp_state_t;

extern lp_state_t lp;

// save track i's RAM loop to the SD library (LOOP_NNNN.RAW + .JSN). Returns 0
// on success, -1 if the track is empty or the write failed. Call from UI task.
int looper_save_track(int i);
