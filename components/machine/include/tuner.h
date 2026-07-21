#pragma once
#include <stdint.h>
#include <stdbool.h>

// tuner — a machine-independent chromatic TUNER on the LINE-IN bus. Same shape
// as beatlisten (see beatlisten.h): a core input tap in the audio task, the
// heavy work on an unpinned prio-4 task, a polled status struct for the UI.
// The DSP is the shared util/pitch_detect YIN engine, so the tuner and Keys'
// sample auto-tune agree by construction.
//
// OFF BY DEFAULT and lazily allocated (~32 KB PSRAM + ~8 KB scratch on first
// enable) — the System > Tuner page turns it on when you open it and off when
// you leave, so it costs one branch per audio block the rest of the time.

typedef struct {
    bool  on;        // service enabled
    bool  have;      // currently hearing a pitch (false = silence/unpitched)
    float hz;        // smoothed fundamental
    int   midi;      // nearest MIDI note
    float cents;     // offset from that note, -50..+50 (the needle)
    float conf;      // 0..1
    int   cost_us;   // detection cost per window, for the load-meter habit
} tuner_status_t;

// audio task, once per block, unconditional (OFF = one branch + return).
// No SD, no blocking, no allocation — same rules as machine process().
void tuner_push(const int32_t in[64]);

// Enable/disable. The first enable allocates; a failed alloc fails soft (stays
// OFF). Safe to call repeatedly. UI/task context — not the audio task.
void tuner_set_enabled(bool on);
bool tuner_get_enabled(void);

void tuner_get_status(tuner_status_t *out);
