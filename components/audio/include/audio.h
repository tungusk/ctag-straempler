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
void audio_remote_trig_debug(int t, uint32_t *now, uint32_t *until, uint32_t *hz);
// teleremote: override CV channel ch (0..7) with v (0..4095) for ms
// milliseconds — the audio task substitutes it for the ADC reading, then
// decays back to the physical knob (a stale web value can never pin a knob)
void audio_remote_cv(int ch, int v, int ms);

// output bounce — record the active machine's OUTPUT bus to a pool REC_ take
// (any machine). Reuses the recording service, fed `out` instead of line-in.
// Global output mute, slewed (~2 ms) so it fades rather than clicks. The OTA
// handler asserts it for the whole flash: the image stream + reboot otherwise
// dump garbage into the rack. Nothing clears it before the reboot on purpose.
void audio_output_mute(bool on);

void audio_bounce_start(void);
void audio_bounce_stop(void);
bool audio_bounce_active(void);

// broadcast — a dedicated socket server (port 8000) streams the live output bus
// as WAV to one browser/VLC client. OFF by default: its 12 KB task stack is
// INTERNAL RAM, which the tracker's render task needs; enable on demand (System→
// Settings, POST /bcast/enable, or the boot "broadcast" setting). Start/stop is
// safe only AFTER initWifi() (the task calls socket()/isWiFiConnected()).
void audio_broadcast_set_enabled(bool on); // spawn/tear-down the server task
bool audio_broadcast_enabled(void);        // the listener task is up (intent)
bool audio_broadcast_active(void);     // a client is currently connected
const char *audio_broadcast_diag(void); // last MP3-path error ("ok" if none)
uint32_t audio_proc_us(void);           // smoothed machine process() cost, us (1450 = 100%)
// Peak block cost since the last read; pass clear=true to arm the next window.
// Catches the single overrunning block a click actually is — the EMA cannot.
uint32_t audio_proc_peak_us(bool clear);
// PEAK BLOCK-TO-BLOCK INTERVAL of the audio loop, us, peak-hold, cleared on read.
// The counterpart to audio_proc_peak_us: that one catches a block that took too
// LONG, this one catches the task coming back too LATE (blocked on sd_lock, on
// I2S, or halted by an internal-flash write disabling the instruction cache).
// One block is ~1450 us; the excess over that is how long the audio was gone.
// Cheap and always on — a stall nobody is watching for is the whole point.
uint32_t audio_loop_gap_us(bool clear);
// last AUTOSAVE.JSN write duration + how many have happened, so an audible burst
// can be lined up against a real card write instead of inferred
void audio_note_save(uint32_t us);      // called by the saver
void audio_save_stats(uint32_t *us, uint32_t *count);
uint32_t audio_broadcast_enc_us(void);  // smoothed shine cost per 26.1ms pass

// FX-CHAIN METERS, per stage of the shared rack (components/fxrack measures and
// reports; the peak-hold storage is here so /status still links in the
// machine-less proof build). Stage indices are fxrack's FXST_*: 0 rack input,
// 1 after FX1, 2 after FX2, 3 chain end (post-reverb, pre-limiter).
//   pk = peak level, % of full scale — >100 means the chain rides the soft
//        limiter, which the output VU cannot show (it reads the limiter's OUTPUT).
//   jp = biggest per-channel sample-to-sample step, % of full scale — a click's
//        signature, and it spans block seams, so a starvation glitch shows too.
// Both peak-hold, cleared on read: ONE poller owns them (the test rig / web UI).
// ARMED ON DEMAND: measuring is per-sample work in the shared audio path, so it
// is OFF unless something is actively polling. A CLEARING read (clear=true, what
// a poller does) arms it for a few seconds; a peek does not. The producers check
// audio_fx_meters_armed() and skip the scan otherwise. A poll loop keeps it
// alive and it goes quiet on its own — over REST that is GET /status?fx=1.
// So the FIRST poll of a run reads zeros: poll continuously, don't sample once.
#define AUDIO_FX_STAGES 4
bool audio_fx_meters_armed(void);                          // producers gate on this
void audio_fx_report(int stage, int pk_pct, int jp_pct);   // audio task only
void audio_fx_meters(int *pk, int *jp, bool clear);        // pk/jp hold >= 4 ints, either may be NULL

// REVERB TANK diagnostics, for the "burst of noise 1-2 s into the first play
// after changing reverb type" (Arlo 2026-07-26). The fxst taps see dry+wet
// mixed, so neither can see the tank alone:
//   wpk   peak of the tank's WET output, % of full scale, peak-hold. The tank
//         has no limiter of its own; >100 means it is producing overload that
//         the chain's soft clipper then hides.
//   nan   COUNT of NaN-guard flushes since boot (never cleared). Each one is an
//         unchunked ~170 KB PSRAM memset in the AUDIO task — the exact stall
//         the chunked clear exists to avoid, and it would sound like a burst.
//         A non-zero count is the smoking gun; a static count exonerates it.
void audio_rv_report(int wet_pk_pct, bool nan_flush);      // audio task only
void audio_rv_meters(int *wet_pk, uint32_t *nan_count, bool clear_pk);

// soft MIDI (web bridge: musical typing / WebMIDI). Machines with pitch read
// gate()/note() next to their CV1/TR1 inputs; MIDI wins while notes are held.
void audio_midi_note_on(int note, int vel);
void audio_midi_note_off(int note);
void audio_midi_all_off(void);
void audio_midi_touch(void);           // heartbeat (liveness stamp)
bool audio_midi_gate(void);            // any note held + bridge alive
int  audio_midi_note(void);            // current note (last priority), -1 none

// icecast push — the module as a SOURCE client (mono 96k MP3 to a mountpoint).
// One push at a time, mutually exclusive with a :8000 listener. start returns
// 0 ok / -1 already running / -2 bad args / -3 task create failed.
int  audio_icepush_start(const char *host, int port, const char *mount,
                         const char *pass, const char *name);
void audio_icepush_stop(void);
bool audio_icepush_running(void);      // user intent (start..stop)
bool audio_icepush_connected(void);    // handshake accepted, streaming now
const char *audio_icepush_err(void);   // last error ("" if none)
uint32_t audio_icepush_retries(void);
