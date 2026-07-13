#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "clock.h"

// M5 glitch — live-input stutter/beat-repeat. Continuously captures line-in
// into a rolling ring; on trigger it grabs the most recent window and loops
// it (with pitch + reverse) until released. No SD. Engine (audio task) owns
// the ring + window; the UI writes params/flags.

#define GL_RATE        44100
#define GL_RING_FRAMES (GL_RATE * 2)          // 2 s stereo ring
#define GL_MAX_WIN     (GL_RATE / 2)          // 500 ms max window (frames)

typedef struct {
    int16_t *ring;                // PSRAM stereo ring, GL_RING_FRAMES*2
    uint32_t wpos;                // ring write head (frames)
    int16_t *win;                 // PSRAM captured window, GL_MAX_WIN*2
    uint32_t win_len;             // captured window length (frames)

    volatile bool stutter;        // currently glitching
    volatile bool latch;          // TR2 latch (hands-free stutter)
    double play_pos;              // window read head
    float  inc;                   // pitch increment

    // params (UI writes, engine reads)
    volatile int  win_ms;         // 20..500 window length (knob6, free mode)
    volatile int  pitch_cv;       // 0..4095 (knob7)
    volatile bool reverse;        // play the window reversed
    volatile int  level;          // 0..255 (CV1 jack)

    // beat sync — the shared conditioned front-end (clock.h): floor-Schmitt
    // replaces the fixed 1500/800 thresholds that misfired on attenuated CV
    clockin_t         ci;
    volatile bool sync;           // window length follows the clock
    volatile int  clk_src;        // clock CV channel (0..7), default CV8
    volatile int  division;       // 0=1/4, 1=1/8, 2=1/16, 3=1/32 note
} gl_state_t;

extern gl_state_t gl;
