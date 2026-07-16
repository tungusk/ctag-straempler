#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// core audio transport: one block = BUF_SZ interleaved int32 slots
#define BUF_SZ 64
#define SAMPLE_RATE 44100

// Upstream flagged channels 3/4 (idx 2/3) as inverting op-amps, but this unit
// reads them straight (verified 2026-07-05: same CV source, meter 1 rose while
// meter 3 fell under the old correction) — same story as the upstream 5/6 ADC
// swap this board doesn't have. idx3 (CV4, broken jack) flipped together with
// idx2: same analog block; revisit when the jack is repaired.
static const bool cv_bipolar[8] = {false, false, false, false, false, false, false, false};
static inline uint16_t cv_corrected(int src, const uint16_t *d) {
    return cv_bipolar[src] ? (uint16_t)(4095u - d[src]) : d[src];
}

typedef struct {
    char v0[32];
    char v1[64];   // diag string; 32 truncated multi-voice diags (dualdeck)
    uint16_t cv[8];
    uint8_t trig;      // raw gate levels, bit0=TR1 bit1=TR2 (active low)
    // rough STEREO VU: decayed per-block peak per channel, 0..255 (peak >> 7
    // of the 16-bit magnitude). Order: inL, inR, outL, outR. A signal-present
    // meter, not a tool.
    uint8_t vu[4];
} audio_status_t;

void initAudio(void);
void audio_get_status(audio_status_t *out);
void audio_get_cv(uint16_t out[8]);
// machines report their display names through this (spinlock-protected)
void audio_status_set_voices(const char *v0, const char *v1);
// teleremote: assert trigger input t (0/1) in software for ms milliseconds —
// the audio task pulls the bit low (= active) alongside the hardware gate
void audio_remote_trig(int t, int ms);
// teleremote: override CV channel ch (0..7) with v (0..4095) for ms
// milliseconds — the audio task substitutes it for the ADC reading, then
// decays back to the physical knob (a stale web value can never pin a knob)
void audio_remote_cv(int ch, int v, int ms);

// output bounce — record the active machine's OUTPUT bus to a pool REC_ take
// (any machine). Reuses the recording service, fed `out` instead of line-in.
void audio_bounce_start(void);
void audio_bounce_stop(void);
bool audio_bounce_active(void);

// broadcast — a dedicated socket server (port 8000) streams the live output bus
// as WAV to one browser/VLC client. Call audio_broadcast_init() AFTER initWifi().
void audio_broadcast_init(void);       // start the server task (post-WiFi only)
bool audio_broadcast_active(void);     // a client is currently connected
const char *audio_broadcast_diag(void); // last MP3-path error ("ok" if none)
uint32_t audio_broadcast_enc_us(void);  // smoothed shine cost per 26.1ms pass
