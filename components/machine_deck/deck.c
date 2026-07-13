// Deck engine — SD-streamed track playback, varispeed, phase-locked to the
// external clock. The reader task owns ALL file I/O; process() only consumes
// the PSRAM ring and flips seek flags the reader acts on.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "fileio.h"
#include "sd_lock.h"
#include "deck_priv.h"

static const char *TAG = "DECK";

dk_state_t dk;
const float dk_ppb[6] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};

// smooth SYNC catch-up: drain sync_slew (frames) at a capped per-frame rate bend
#define SYNC_SLEW_GAIN 0.0006f   // drains ~1667-frame TC once under the cap
#define SYNC_SLEW_MAX  0.15f     // max +/-15% rate bend (~2.4 semitones), no dropout
const char *const dk_ppb_names[6] = {"1 per 4 beats", "1 per 2 beats", "1 per beat", "2 per beat", "4 per beat", "8 per beat"};

static volatile bool s_run = false, s_alive = false;
static volatile bool s_track_req = false;
static char s_pending[DK_NAME_LEN];

// ---- reader task ------------------------------------------------------------
static void reader_task(void *pv)
{
    FILE *f = NULL;
    char cur[DK_NAME_LEN] = "";
    // DMA-capable internal RAM per the SD house rule (with CAPS_ALLOC plain
    // malloc is internal anyway; explicit caps guard against config drift)
    int16_t *chunk = heap_caps_malloc(4096 * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    s_alive = true;

    while (s_run) {
        if (s_track_req) {
            s_track_req = false;
            if (f) { sd_lock_take(); fclose(f); sd_lock_give(); f = NULL; }
            strlcpy(cur, s_pending, sizeof(cur));
            if (cur[0]) {
                char path[64];
                snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", cur);
                sd_lock_take();
                f = fopen(path, "rb");
                if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); dk.file_frames = (uint32_t)(sz / 4); }
                sd_lock_give();
                if (!f) { ESP_LOGE(TAG, "open %s failed", path); dk.file_frames = 0; }
                dk.wpos = 0;
                dk.rpos_i = 0;
                dk.rpos_f = 0;
            }
        }
        if (f && dk.seek_req) {
            vTaskDelay(1);                      // 1 tick settle; pdMS_TO_TICKS(4)==0 at 100Hz
            dk.seek_req = false;
            uint32_t to = dk.seek_to;           // latest wins if requests raced
            if (to >= dk.file_frames) to = 0;
            sd_lock_take();
            fseek(f, (long)to * 4, SEEK_SET);
            sd_lock_give();
            dk.wpos = to;                       // ring restarts from the seek point
            dk.rpos_i = to;                     // reader is the ONLY seek-writer of rpos
        }
        if (f && !s_track_req && !dk.seek_req) {
            uint32_t lead = dk.wpos - dk.rpos_i;
            if (dk.wpos < dk.file_frames && lead < DK_RING_FRAMES - 4096) {
                uint32_t want = dk.file_frames - dk.wpos;
                if (want > 4096) want = 4096;
                sd_lock_take();
                size_t got = fread(chunk, 4, want, f);
                sd_lock_give();
                if (got > 0) {
                    uint32_t w = dk.wpos % DK_RING_FRAMES;
                    uint32_t first = DK_RING_FRAMES - w;
                    if (first > got) first = got;
                    memcpy(dk.ring + w * 2, chunk, first * 4);
                    if (first < got) memcpy(dk.ring, chunk + first * 2, (got - first) * 4);
                    dk.wpos += got;
                }
                if (dk.loading && (dk.wpos - dk.rpos_i >= DK_LOW_WATER ||
                                   dk.wpos >= dk.file_frames))
                    dk.loading = false;
                continue;              // keep filling without the delay below
            }
            if (dk.loading && dk.wpos >= dk.file_frames) dk.loading = false;
        }
        vTaskDelay(1);   // >=1 tick: pdMS_TO_TICKS(5)==0 at 100Hz = busy-spin
    }
    if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
    free(chunk);
    s_alive = false;
    vTaskDelete(NULL);
}

// ---- UI-side controls -------------------------------------------------------
int deck_load_track(const char *name)
{
    dk.playing = false;
    dk.loading = true;
    dk.track_bpm = 0;
    dk.grid_offset = 0;
    dk.phase_int = 0.0f;                         // new track: fresh PLL integrator
    dk.phase_offset = 0.0f;                       // and clear any manual nudge
    dk.sync_slew = 0.0f;
    strlcpy(dk.track, name, sizeof(dk.track));
    strlcpy(s_pending, name, sizeof(s_pending));
    s_track_req = true;

    // cached analysis from the sidecar, if this track has been analysed before
    char jp[64];
    int dver = 0;                    // sidecar analysis version (missing = v1)
    snprintf(jp, sizeof(jp), "/sdcard/usr/%s.JSN", name);
    cJSON *root = readJSONFileAsCJSON(jp);
    if (root) {
        cJSON *j;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "bpm")) && cJSON_IsNumber(j))
            dk.track_bpm = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "grid")) && cJSON_IsNumber(j))
            dk.grid_offset = (uint32_t)j->valuedouble;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "dver")) && cJSON_IsNumber(j))
            dver = j->valueint;
        cJSON_Delete(root);
    }
    if (dk.an_state != DK_AN_RUNNING) dk.an_state = DK_AN_IDLE;
    // auto-analyze when enabled and the sidecar had no bpm OR carries a
    // pre-ladder (v1) result — the library upgrades itself one load at a
    // time; a current (dver >= 2) track never re-analyzes automatically
    if (dk.auto_an && (dk.track_bpm <= 0 || dver < 2)) {
        // analyze now (runs fine alongside playback); if a previous track's
        // run is still going, queue behind it (menu tick fires it when the
        // runner exits)
        if (deck_analyze_start() != 0) dk.an_auto_req = true;
    }
    return 0;
}

void deck_restart(void)
{
    if (!dk.track[0]) return;
    dk.loading = true;                    // engine mutes while the ring refills
    dk.phase_offset = 0.0f;               // hard reset-to-cue clears the nudge
    dk.phase_int = 0.0f;                  // and the loop integrator (phase jumped)
    dk.sync_slew = 0.0f;                  // and any in-flight SYNC catch-up
    dk.seek_to = (dk.grid_offset < dk.file_frames) ? dk.grid_offset : 0;
    dk.seek_req = true;                   // reader applies rpos
    dk.playing = true;
}

void deck_toggle_play(void)
{
    if (dk.playing) { dk.playing = false; return; }
    if (!dk.track[0]) return;
    // resume from the current (possibly scrubbed) position — the cue point;
    // TR1 is the "from the top of the grid" restart
    uint32_t to = dk.rpos_i;
    if (to >= dk.file_frames)
        to = (dk.grid_offset < dk.file_frames) ? dk.grid_offset : 0;
    dk.loading = true;
    dk.phase_int = 0.0f;                  // fresh PLL integrator on resume
    dk.sync_slew = 0.0f;
    dk.seek_to = to;
    dk.seek_req = true;                   // reader applies rpos
    dk.playing = true;
}

void deck_seek_beats(int beats)
{
    if (!dk.track[0] || !dk.file_frames) return;
    uint32_t beat_tf = (dk.track_bpm > 20.0f)
        ? (uint32_t)(60.0f * DK_RATE / dk.track_bpm)
        : DK_RATE;                              // no grid yet: 1 s steps
    // snap current position to the nearest grid beat, then step whole beats —
    // scrubbing during sync'd playback lands phase-true by construction
    int64_t rel = (int64_t)dk.rpos_i - (int64_t)dk.grid_offset;
    int64_t idx = (rel >= 0) ? (rel + beat_tf / 2) / beat_tf
                             : -(((-rel) + beat_tf / 2) / beat_tf);
    int64_t tgt = (int64_t)dk.grid_offset + (idx + beats) * (int64_t)beat_tf;
    if (tgt < 0) tgt = 0;
    if (tgt > (int64_t)dk.file_frames - 1) tgt = (int64_t)dk.file_frames - 1;
    dk.loading = true;                          // brief mute while the ring refills
    dk.phase_int = 0.0f;                         // phase ref jumped: reset integrator
    dk.sync_slew = 0.0f;
    dk.seek_to = (uint32_t)tgt;
    dk.seek_req = true;                         // reader applies rpos; play state stays
}

// Hard-snap the playback phase so the track grid aligns to the external clock
// NOW, instead of waiting for the slow loop to pull in. Snaps to the current
// nudge offset target (keeps phase_offset). Called from the audio task on a TR2
// long-hold — only float math + a seek request, so it's safe there.
void deck_sync_now(void)
{
    if (!dk.track[0] || !dk.file_frames) return;
    if (!dk.sync || !dk.clk.locked || dk.clk.period == 0 || dk.track_bpm <= 20.0f) return;
    uint32_t beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
    float seg_tf = (float)beat_tf / dk_ppb[dk.ppb_idx] * dk.speed_mult;   // frames/pulse
    if (seg_tf < 1.0f) return;
    float p_ext = (float)dk.clk.since / (float)dk.clk.period;
    if (p_ext > 1.0f) p_ext = 1.0f;
    int64_t rel = (int64_t)dk.rpos_i - (int64_t)dk.grid_offset;
    float p_trk = fmodf((float)rel, seg_tf) / seg_tf;
    if (p_trk < 0) p_trk += 1.0f;
    float dphase = (p_ext - dk.phase_offset) - p_trk;   // move to the offset target
    dphase -= floorf(dphase);                           // wrap to [0,1)
    if (dphase > 0.5f) dphase -= 1.0f;                  // nearest, no whole-pulse jump
    // smooth catch-up: shift dphase*seg_tf frames via a brief rate bend applied
    // in the playback loop — no seek, no ring refill, so no dropout. +ve = play
    // ahead to catch up; -ve = ease back (play slightly slow). The PLL finishes.
    dk.sync_slew = dphase * seg_tf;
}

// ---- engine -----------------------------------------------------------------
static esp_err_t deck_start(void)
{
    memset(&dk, 0, sizeof(dk));
    s_pending[0] = 0;
    s_track_req = false;
    dk.ring = heap_caps_malloc((size_t)DK_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!dk.ring) { ESP_LOGE(TAG, "PSRAM ring alloc failed"); return ESP_ERR_NO_MEM; }
    dk.sync = true;
    dk.loop = true;
    dk.auto_an = true;       // auto-analyze unanalyzed tracks on load
    dk.clk_src = 7;          // CV8, same convention as glitch
    dk.ppb_idx = 4;          // 4 pulses per beat — the modular norm (4 PPQN)
    dk.rate = 1.0f;
    dk.rate_sm = 1.0f;
    dk.clk_base = 4095;      // floor tracker converges down on first reads
    clock_reset(&dk.clk);
    s_run = true;
    // unpinned: file-reading tasks pinned to core 0 cause WiFi audio clicks
    xTaskCreate(reader_task, "deck_reader", 4096, NULL, 6, NULL);
    audio_status_set_voices("deck", "");
    return ESP_OK;
}

static void deck_stop(void)
{
    dk.playing = false;
    s_run = false;
    for (int i = 0; i < 100 && s_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free(dk.ring);
    dk.ring = NULL;
}

static void deck_process(int32_t out[MACHINE_BLOCK],
                         const int32_t in[MACHINE_BLOCK],
                         const machine_io_t *io)
{
    if (!dk.ring) return;

    // transport gates. TR1 falling edge = reset-to-cue (grid downbeat). TR2:
    // short tap = play/pause (fires on release); long hold (>0.6s) = SYNC (snap
    // grid to clock) and the hold suppresses the release-pause. Flags only; the
    // reader task seeks. prev_trig seeded idle-high so block 1 sees no phantom.
    static uint8_t prev_trig = 0x03;
    static uint32_t tr2_hold = 0;      // frames TR2 has been held low (0 = up)
    static bool tr2_synced = false;    // long-hold already fired SYNC this press
    const uint32_t TR2_LONG = (uint32_t)(0.6f * DK_RATE);
    const int nfr = MACHINE_BLOCK / 2;

    uint8_t fell = prev_trig & (~io->trig_level) & 0x03;    // 1->0 this block
    uint8_t rose = (~prev_trig) & io->trig_level & 0x03;    // 0->1 this block
    prev_trig = io->trig_level;

    if (fell & 1) deck_restart();                           // TR1: reset-to-cue

    if (fell & 2) { tr2_hold = nfr; tr2_synced = false; }   // TR2 press begins
    else if (tr2_hold > 0 && !(io->trig_level & 2)) {       // still held low
        tr2_hold += nfr;
        if (!tr2_synced && tr2_hold >= TR2_LONG) { deck_sync_now(); tr2_synced = true; }
    }
    if (rose & 2) {                                         // TR2 release
        if (!tr2_synced && tr2_hold > 0) deck_toggle_play();  // short tap = pause/resume
        tr2_hold = 0;
    }

    dk.pitch_cv = io->cv[6];   // knob7 = free-run rate when sync is off

    // DJ filter from knob6: centre dead zone = bypass; left half sweeps a
    // low-pass down (12 kHz -> 80 Hz), right half a high-pass up (30 Hz ->
    // 6 kHz). Exponential sweeps; coefficient slewed per block (no zipper).
    int fcv = io->cv[5];
    dk.filt_cv = fcv;
    int mode = 0;
    float fc = 0;
    if (fcv < 2048 - 150) {              // LP zone
        mode = 1;
        float t = (float)fcv / (2048.0f - 150.0f);          // 1..0 as knob goes left
        fc = 80.0f * powf(150.0f, t);                       // 80 Hz .. 12 kHz
    } else if (fcv > 2048 + 150) {       // HP zone
        mode = 2;
        float t = (float)(fcv - 2048 - 150) / (4095.0f - 2048.0f - 150.0f);
        fc = 30.0f * powf(200.0f, t);                       // 30 Hz .. 6 kHz
    }
    dk.flt_mode = mode;
    float f_target = mode ? 2.0f * sinf(3.14159265f * fc / (float)DK_RATE) : 0;
    if (f_target > 1.2f) f_target = 1.2f;
    dk.flt_f += 0.2f * (f_target - dk.flt_f);
    const float q = 0.9f;                // mild resonance, DJ-ish

    // playback rate
    float rate = 1.0f;
    uint32_t beat_tf = 0;      // track frames per beat at nominal rate
    if (dk.track_bpm > 20.0f) beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
    if (dk.sync) {
        // knob7 = musical speed while synced: half-time / straight / double,
        // snapped so the lock stays meaningful (continuous rates would slide
        // off the grid). Free-run (sync off) keeps the continuous knob.
        float m = 1.0f;
        if (dk.pitch_cv < 1024) m = 0.5f;
        else if (dk.pitch_cv > 3072) m = 2.0f;
        dk.speed_mult = m;
        if (dk.clk.locked && dk.clk.period > 0 && beat_tf > 0) {
            float ppb = dk_ppb[dk.ppb_idx];
            // pulse-level phase lock (works for any mult/div): compare phase
            // within one external pulse against the track's matching segment
            float seg_tf = (float)beat_tf / ppb * m;           // track frames per pulse
            float base = seg_tf / (float)dk.clk.period;        // nominal rate
            float p_ext = (float)dk.clk.since / (float)dk.clk.period;
            if (p_ext > 1.0f) p_ext = 1.0f;
            float p_trk = fmodf((float)((int64_t)dk.rpos_i - (int64_t)dk.grid_offset), seg_tf) / seg_tf;
            if (p_trk < 0) p_trk += 1.0f;
            // fixed-TIME lock lead: the output chain lands beats a constant
            // ~10.5 ms late (I2S/DAC latency + block-quantized edge capture +
            // grid-anchor bias — measured +10.6 ms median, std 4 ms, over 480
            // beats). Lead the target by the same amount, expressed in pulse
            // phase so it tracks any clock rate. Bench sign check 2026-07-12.
            // constant tuned empirically by capture (cmd 10.5 -> +4.6 ms,
            // cmd 18.5 -> -9.5 ms; zero-crossing ~13 ms). The between-settle
            // baseline wanders ±3-5 ms, so exact zero is a moving target —
            // this centers it (musically dead-on)
            #define DK_LAG_LEAD_FR (0.0131f * 44100.0f)
            float lead = DK_LAG_LEAD_FR / (float)dk.clk.period;
            float err = p_ext - p_trk - dk.phase_offset + lead;   // NUDGE trims the lock point
            err -= floorf(err);                            // wrap to [0,1)
            if (err > 0.5f) err -= 1.0f;                    // nearest, in [-0.5,0.5)
            // PI loop filter. The P term (0.08) chases phase fast; the I term
            // accumulates to cancel the residual frequency error a P-only loop
            // leaves as slow drift (track_bpm is never exact). Integrator is
            // clamped to a +/-4% rate-trim band (analysis is within a few %),
            // which also bounds wind-up. Reset on unlock / seek / load.
            // LEAKY integrator. The 0.9999 leak bleeds off any slow bias in the
            // phase measurement (the p_ext clamp above isn't perfectly zero-mean)
            // so it can't wind up over minutes — that wind-up was the "audio
            // degrades after a while". Gentle gain (0.0002) so it doesn't hunt,
            // yet still trims the residual frequency error (drift).
            dk.phase_int = dk.phase_int * 0.9999f + 0.0002f * err;
            if (dk.phase_int > 0.04f) dk.phase_int = 0.04f;
            else if (dk.phase_int < -0.04f) dk.phase_int = -0.04f;
            rate = base * (1.0f + 0.08f * err + dk.phase_int);
            dk.phase_err = err;
        } else {
            rate = m;          // no clock yet: play straight (times the speed knob)
            dk.phase_int = 0.0f;   // start each lock from a clean integrator
        }
    } else {
        // free run: knob7, unity plateau around centre (same feel as glitch)
        int pc = dk.pitch_cv;
        if (pc >= 1843 && pc <= 2253) rate = 1.0f;
        else if (pc > 2253) rate = 1.0f + (float)(pc - 2253) / 1842.0f;
        else                rate = 0.5f + (float)pc / 1843.0f * 0.5f;
    }
    if (rate < 0.25f) rate = 0.25f;
    if (rate > 2.5f) rate = 2.5f;
    // ~18 ms slew: clock edge jitter shifts the target rate in steps; the
    // slew turns that into inaudible drift instead of pitch warble
    dk.rate_sm += 0.08f * (rate - dk.rate_sm);
    rate = dk.rate_sm;
    dk.rate = rate;

    int frames = MACHINE_BLOCK / 2;
    static float last_l = 0, last_r = 0;
    bool starved = false;
    for (int fno = 0; fno < frames; fno++) {
        uint32_t avail_to = dk.wpos < dk.file_frames ? dk.wpos : dk.file_frames;
        bool can_play = dk.playing && !dk.loading && dk.rpos_i + 1 < avail_to;
        // mid-track mute with the reader as the limiter = ring underrun;
        // counted per block into /status so click reports are attributable
        if (!can_play && dk.playing && !dk.loading && !dk.seek_req &&
            dk.file_frames && dk.rpos_i + 1 < dk.file_frames)
            starved = true;
        // declick both edges: gain ramps in on resume; on mute the last
        // sample decays out instead of stepping to zero (each scrub detent
        // used to click — "beeps")
        float gt = can_play ? 1.0f : 0.0f;
        dk.out_gain += (gt - dk.out_gain) * 0.015f;
        if (!can_play) {
            if (dk.playing && !dk.loading && dk.file_frames &&
                dk.rpos_i + 1 >= dk.file_frames && !dk.seek_req) {
                if (dk.loop) deck_restart();     // sets flags; reader seeks
                else dk.playing = false;
            }
            last_l *= 0.94f;
            last_r *= 0.94f;
            out[fno * 2]     = ((int32_t)last_l) << 16;
            out[fno * 2 + 1] = ((int32_t)last_r) << 16;
            continue;
        }
        uint32_t i0 = dk.rpos_i % DK_RING_FRAMES;
        uint32_t i1 = (i0 + 1) % DK_RING_FRAMES;
        float fr = (float)dk.rpos_f;
        float l = (float)dk.ring[i0 * 2]     + ((float)dk.ring[i1 * 2]     - (float)dk.ring[i0 * 2])     * fr;
        float r = (float)dk.ring[i0 * 2 + 1] + ((float)dk.ring[i1 * 2 + 1] - (float)dk.ring[i0 * 2 + 1]) * fr;
        if (mode) {                       // Chamberlin SVF per channel
            dk.lp_l += dk.flt_f * dk.bp_l;
            float hi_l = l - dk.lp_l - q * dk.bp_l;
            dk.bp_l += dk.flt_f * hi_l;
            l = (mode == 1) ? dk.lp_l : hi_l;
            dk.lp_r += dk.flt_f * dk.bp_r;
            float hi_r = r - dk.lp_r - q * dk.bp_r;
            dk.bp_r += dk.flt_f * hi_r;
            r = (mode == 1) ? dk.lp_r : hi_r;
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
        } else {
            dk.lp_l = l; dk.bp_l = 0;     // park state at the signal: no thump
            dk.lp_r = r; dk.bp_r = 0;     // when the filter re-engages
        }
        last_l = l * dk.out_gain;
        last_r = r * dk.out_gain;
        out[fno * 2]     = ((int32_t)last_l) << 16;
        out[fno * 2 + 1] = ((int32_t)last_r) << 16;
        // smooth SYNC: bleed the phase correction in as a capped rate bend
        // (rate stays > 0 since rate>=0.25 and |sl|<=0.15, so no rewind needed)
        float sl = dk.sync_slew * SYNC_SLEW_GAIN;
        if (sl > SYNC_SLEW_MAX) sl = SYNC_SLEW_MAX;
        else if (sl < -SYNC_SLEW_MAX) sl = -SYNC_SLEW_MAX;
        dk.sync_slew -= sl;
        dk.rpos_f += rate + sl;
        while (dk.rpos_f >= 1.0) { dk.rpos_f -= 1.0; dk.rpos_i++; }
    }
    if (starved) dk.dbg_starve++;

    // clock conditioning: track the channel floor (dips follow instantly,
    // drifts back up slowly), Schmitt relative to it, and feed the detector
    // a clean synthesized square — works on any channel at any attenuation
    // scale the detector's sanity gate to the expected PULSE rate — with the
    // default 20..300 BPM gate a 4 PPQN clock's every interval is rejected
    // and the BPM is assembled purely from missed-edge garbage
    float gppb = dk_ppb[dk.ppb_idx];
    dk.clk.period_min = (uint32_t)(44100.0f * 60.0f / (320.0f * gppb));
    dk.clk.period_max = (uint32_t)(44100.0f * 60.0f / (15.0f * gppb));

    int ccv = io->cv[dk.clk_src & 7];
    if (ccv < dk.clk_base) dk.clk_base = ccv;
    else if (dk.clk_base < 4095) dk.clk_base++;
    dk.dbg_since += (uint32_t)frames;
    if (!dk.clk_high) {
        if (ccv >= dk.clk_base + 900) {
            dk.clk_high = true;
            dk.dbg_edges++;                    // raw fire, pre-gate
            dk.dbg_iv = dk.dbg_since;
            dk.dbg_since = 0;
        }
    } else if (ccv < dk.clk_base + 350) {
        dk.clk_high = false;
    }
    uint16_t synth = dk.clk_high ? 4095 : 0;
    for (int fno = 0; fno < frames; fno++) clock_tick(&dk.clk, synth);
}

// ---- preset -------------------------------------------------------------------
static cJSON *deck_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "track", dk.track);
    cJSON_AddBoolToObject(o, "sync", dk.sync);
    cJSON_AddBoolToObject(o, "loop", dk.loop);
    cJSON_AddBoolToObject(o, "auto_an", dk.auto_an);
    cJSON_AddNumberToObject(o, "clk_src", dk.clk_src);
    cJSON_AddNumberToObject(o, "ppb", dk.ppb_idx);
    return o;
}

static void deck_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sync"))) dk.sync = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "loop"))) dk.loop = cJSON_IsTrue(j);
    // must land before the track restore below so it gates the boot-time load
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "auto_an"))) dk.auto_an = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j)) dk.clk_src = j->valueint & 7;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ppb")) && cJSON_IsNumber(j)) {
        dk.ppb_idx = j->valueint;
        if (dk.ppb_idx < 0) dk.ppb_idx = 0;
        if (dk.ppb_idx > 5) dk.ppb_idx = 5;
    }
    // only (re)load when the track actually changes — a remote "Apply" that
    // only touched sync/ppb/clk_src must NOT reload the track (that re-triggers
    // the ring refill + PLL cold-relock and makes the deck sound like it reset)
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "track")) && cJSON_IsString(j) && j->valuestring[0]
        && strcmp(j->valuestring, dk.track) != 0)
        deck_load_track(j->valuestring);         // also restores cached bpm/grid
}

extern const machine_ui_t deck_menu_ui;

const machine_t machine_deck = {
    .name = "Deck",
    .start = deck_start,
    .stop = deck_stop,
    .process = deck_process,
    .preset_save = deck_preset_save,
    .preset_load = deck_preset_load,
    .ui = &deck_menu_ui,
};
