#pragma once
#include <stdint.h>
#include <stdbool.h>

// sampplay — STREAMING WINDOW PLAYER for pool samples.
//
// Loops [in, out) of a file on the SD card straight from SD, through a PSRAM
// ring filled by its own reader task. Built (2026-09-12) because the two
// "silent utility" machines both needed to be audible and neither could hold
// its material in RAM: a 90 s stereo Freesound preview is ~16 MB, and the
// Editor's whole point is files longer than that.
//
// The shape is Radio's network ring (radio.c) with an SD reader in front of it,
// and the window semantics are Tape's crop (in/out, loop at the end).
//
// Ownership, so the audio task never blocks:
//   render() writes rpos      (audio task)
//   reader   writes wpos+gate (reader task)
//   caller   writes in/out/flush
// A window change raises `flush`; the reader drops the gate, rewinds, refills
// and raises it again. render() outputs silence while the gate is down, so a
// seek costs a few ms of quiet rather than a glitch.

typedef struct sampplay_s sampplay_t;

// ring_frames 0 = the default ~1 s. Allocates the ring in PSRAM and starts the
// reader task; NULL if either fails.
sampplay_t *sampplay_create(uint32_t ring_frames);
void        sampplay_destroy(sampplay_t *p);

// Open a pool id (resolved through sample_resolve, so any of .RAW/.WAV/.AIFF).
// The window resets to the whole file. 0 ok, -1 on failure.
int         sampplay_open(sampplay_t *p, const char *name);
void        sampplay_close(sampplay_t *p);
bool        sampplay_is_open(const sampplay_t *p);
uint32_t    sampplay_frames(const sampplay_t *p);   // total frames of the open file

// Loop bounds. Clamped to the file, and to at least one chunk apart.
void        sampplay_window(sampplay_t *p, uint32_t in, uint32_t out);
void        sampplay_play(sampplay_t *p, bool on);
bool        sampplay_playing(const sampplay_t *p);
uint32_t    sampplay_pos(const sampplay_t *p);      // playhead frame, for the UI

// Audio task: write `frames` stereo frames into out[] as the machine ABI wants
// them (out[f*2] = L, out[f*2+1] = R, each int16 << 16). Silence when idle.
void        sampplay_render(sampplay_t *p, int32_t *out, int frames, float gain);
