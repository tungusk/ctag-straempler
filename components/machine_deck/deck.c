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
#include "trig_gate.h"
#include "sampfile.h"
#include "deck_priv.h"

static const char *TAG = "DECK";

dk_state_t dk;
const float dk_ppb[6] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};

// smooth SYNC catch-up: drain sync_slew (frames) at a capped per-frame rate bend
#define SYNC_SLEW_GAIN 0.0006f   // drains ~1667-frame TC once under the cap
#define SYNC_SLEW_MAX  0.15f     // max +/-15% rate bend (~2.4 semitones), no dropout
const char *const dk_ppb_names[6] = {"1 per 4 beats", "1 per 2 beats", "1 per beat", "2 per beat", "4 per beat", "8 per beat"};

// fixed-TIME lock lead: the output chain lands beats a constant few ms late
// (I2S/DAC latency + block-quantized edge capture + grid-anchor bias), so the
// target is led by the same amount, in pulse phase so it tracks any rate.
// RE-TUNED BY CAPTURE 2026-07-13 (Arlo: "the pll sounds slightly off, can hear
// slight offset on the transients"). At 13.1 ms BOTH machines landed ~4.3 ms
// EARLY against the clock (60 s, 125 beats: mean -4.32 ms, std 2.53). The deck's
// calibration curve is ~-1.8 ms of offset per +1 ms of lead, so the lead comes
// DOWN by ~2.5 ms to put the beat on the pulse.
#define DK_LAG_LEAD_FR (0.0068f * 44100.0f)
#define DK_XFADE 256          // ~5.8 ms seam fade — a click-killer, not a blur
#define DK_PICKUP 120         // knob counts of movement that GRAB a loop knob
// PASS-THROUGH pickup on loop EXIT (Arlo: "i want cv7 to have to pickup on loop
// exit or else it jumps the tempo to 2x or .5"). While looping, CV6/CV7 belong
// to the loop (window + length), so on release they sit wherever the loop left
// them — and a mere move-and-grab is not enough: the first nudge would snap the
// value to the knob's physical position, which after a ladder sweep means 2x or
// half speed. So a released knob stays INERT until it comes back THROUGH the
// value the engine is still using; by then it agrees with the sound, and
// nothing can jump.
#define DK_PASSTOL 90         // how close a knob must come to reclaim its param
// Read-ahead while looping. This IS the latency of a length/window change, so
// it wants to be small — but it must stay ABOVE DK_LOW_WATER, or the reader
// parks below the level the engine treats as "buffered" and the ring starves
// continuously (bench: 0.25 s cap -> 32k starve blocks in 20 s). 0.5 s is the
// proven floor; the CV7 responsiveness fix does the rest.
#define DK_LOOP_LEAD (DK_RATE / 2)
#define DK_MOVE_KEEP (DK_RATE / 6)   // ~0.17 s of read-ahead kept on a move: the
                                     // gap between "instant" and "starving"

// playback counter -> FILE frame. The reader owns this mapping; everyone else
// (engine, UI) goes through it. Loop = a mapping, not a cursor wrap.
static inline uint32_t dk_map(uint32_t p)
{
    if (dk.loop_active && dk.rm_at && p >= dk.rm_at && dk.rm_len)
        return dk.rm_start + ((p - dk.rm_p0) % dk.rm_len);   // scheduled window
    if (dk.loop_active && dk.loop_len_fr)
        return dk.loop_start + ((p - dk.map_p0) % dk.loop_len_fr);
    if (dk.tl_len) {                       // track loop: wrap at the last beat
        int64_t rel = (int64_t)dk.map_f0 + (int64_t)(p - dk.map_p0) - (int64_t)dk.tl_start;
        if (rel >= 0) return dk.tl_start + (uint32_t)(rel % (int64_t)dk.tl_len);
    }
    return dk.map_f0 + (p - dk.map_p0);
}

// the active streaming window (KO-II loop, else track loop). false = linear.
static inline bool dk_window(uint32_t *st, uint32_t *len)
{
    if (dk.loop_active && dk.rm_at && dk.wpos >= dk.rm_at && dk.rm_len) {
        *st = dk.rm_start; *len = dk.rm_len; return true;    // scheduled window
    }
    if (dk.loop_active && dk.loop_len_fr) { *st = dk.loop_start; *len = dk.loop_len_fr; return true; }
    if (dk.tl_len && dk_map(dk.rpos_i) >= dk.tl_start) { *st = dk.tl_start; *len = dk.tl_len; return true; }
    return false;
}

// (re)compute the track-loop window: whole beats from the downbeat. Called
// whenever loop / bpm / grid / track change.
static void dk_tl_update(void)
{
    if (!dk.loop || !dk.file_frames || dk.grid_offset >= dk.file_frames) {
        dk.tl_len = 0;
        return;
    }
    uint32_t span = dk.file_frames - dk.grid_offset;
    if (dk.track_bpm > 20.0f) {
        uint32_t beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
        if (beat_tf) {
            uint32_t n = span / beat_tf;               // whole beats only
            if (n) span = n * beat_tf;                 // trim the partial tail
        }
    }
    dk.tl_start = dk.grid_offset;
    dk.tl_len = span;
}

// KNOB PICKUP after a loop (Arlo, ear test: "when leaving loop we need it to
// not immediately apply cv7 because it affects the playback speed"). While
// looping CV6/CV7 belong to the loop (window + length), so on release they sit
// wherever the loop left them — applying them instantly SLAMS the speed and
// the filter. Both stay FROZEN at their pre-loop values until the physical
// knob moves past the deadband; then it takes over smoothly.
//   -2 = live, -1 = armed (seize the reference next block), >=0 = reference
static int s_pk6 = -2, s_pk7 = -2;

static volatile bool s_run = false, s_alive = false;
static volatile bool s_track_req = false;
static char s_pending[DK_NAME_LEN];

// ---- reader task ------------------------------------------------------------
static void reader_task(void *pv)
{
    FILE *f = NULL;
    sampfile_t sfl = {0};          // container descriptor for the open track
    char cur[DK_NAME_LEN] = "";
    // DMA-capable internal RAM per the SD house rule (with CAPS_ALLOC plain
    // malloc is internal anyway; explicit caps guard against config drift)
    int16_t *chunk = heap_caps_malloc(4096 * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    int16_t *tail  = heap_caps_malloc(DK_XFADE * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    uint32_t cur_ff = (uint32_t)-1;    // file frame the handle is parked at
    s_alive = true;

    while (s_run) {
        if (s_track_req) {
            s_track_req = false;
            if (f) { sd_lock_take(); fclose(f); sd_lock_give(); f = NULL; }
            strlcpy(cur, s_pending, sizeof(cur));
            if (cur[0]) {
                char path[64];
                sample_resolve(cur, path, sizeof(path));   // .RAW/.WAV/.AIF(F)
                sd_lock_take();
                f = fopen(path, "rb");
                if (f && sampfile_probe(f, &sfl) != 0) {
                    ESP_LOGE(TAG, "%s: %s", path, sfl.why);   // e.g. "WAV: not 44.1kHz"
                    fclose(f); f = NULL;
                }
                if (f) dk.file_frames = sfl.frames;
                sd_lock_give();
                if (!f) { ESP_LOGE(TAG, "open %s failed", path); dk.file_frames = 0; }
                dk.wpos = 0;
                dk.rpos_i = 0;
                dk.rpos_f = 0;
                dk.map_p0 = 0;
                dk.map_f0 = 0;
                cur_ff = (uint32_t)-1;
                dk.wf_state = (f && dk.file_frames) ? 1 : 0;   // thumbnail pending
                dk.wf_col = 0;
            }
        }
        if (f && dk.seek_req) {
            vTaskDelay(1);                      // 1 tick settle; pdMS_TO_TICKS(4)==0 at 100Hz
            dk.seek_req = false;
            uint32_t to = dk.seek_to;           // latest wins if requests raced
            if (to >= dk.file_frames) to = 0;
            // counters stay MONOTONIC (ring slots depend on them) — a seek
            // rewrites the MAPPING instead of rewinding them
            uint32_t base = dk.wpos + DK_RING_FRAMES;   // fresh, non-aliasing slots
            dk.map_p0 = base;
            dk.map_f0 = to;
            dk.rpos_i = base;                   // reader is the ONLY seek-writer of rpos
            dk.rpos_f = 0;
            dk.wpos = base;
            cur_ff = (uint32_t)-1;              // force a file seek on the next fill
        }
        if (f && !s_track_req && !dk.seek_req) {
            uint32_t win_st = 0, win_len = 0;
            bool lp = dk_window(&win_st, &win_len);
            // SIGNED: if the frontier ever ends up behind the cursor, an
            // unsigned lead underflows to ~4e9, the reader decides it is far
            // ahead and never fills again — a silent permanent starve. Heal it.
            int32_t slead = (int32_t)(dk.wpos - dk.rpos_i);
            if (slead < 0) {
                ESP_LOGW(TAG, "ring lead went negative (%ld) — resyncing", (long)slead);
                dk.wpos = dk.rpos_i;
                cur_ff = (uint32_t)-1;
                slead = 0;
            }
            uint32_t lead = (uint32_t)slead;
            // lead cap: a modest read-ahead normally (>=1 s of history stays
            // resident behind rpos — the loop anchor may sit a beat back), and
            // a SHORT one while looping, because that lead IS the latency of a
            // window move. The ring covers it with already-buffered audio.
            // a KO-II loop keeps a SHORT lead (it is the latency of a window
            // move); the track loop can read far ahead like normal playback
            uint32_t lead_cap = (dk.loop_active && dk.loop_len_fr)
                                    ? DK_LOOP_LEAD
                                    : (DK_RING_FRAMES - DK_RATE - 4096);
            uint32_t ff = dk_map(dk.wpos);
            bool have_room = lp ? (lead < lead_cap)
                                : (dk.wpos < dk.map_p0 + (dk.file_frames - dk.map_f0) &&
                                   lead < lead_cap);
            if (have_room && ff < dk.file_frames) {
                // never read past the window end (loop) or the file end (linear)
                uint32_t limit = lp ? (win_st + win_len - ff) : (dk.file_frames - ff);
                uint32_t want = limit < 4096 ? limit : 4096;
                bool seam = lp && ff == win_st && dk.wpos > dk.map_p0;
                sd_lock_take();
                if (cur_ff != ff) { fseek(f, sf_seek_pos(&sfl, ff), SEEK_SET); cur_ff = ff; }
                size_t got = sampfile_read(f, &sfl, chunk, want);
                cur_ff += got;
                // SEAM: blend the incoming head against the tail CONTINUING past
                // the window end (phase-exact — the cycle keeps its full length)
                if (seam && got > 0) {
                    uint32_t lend = win_st + win_len;
                    uint32_t nx = got < DK_XFADE ? (uint32_t)got : DK_XFADE;
                    if (lend + nx <= dk.file_frames) {
                        fseek(f, sf_seek_pos(&sfl, lend), SEEK_SET);
                        size_t tg = sampfile_read(f, &sfl, tail, nx);
                        for (size_t k = 0; k < tg; k++) {
                            float w = (float)k / (float)DK_XFADE;   // 0 -> 1
                            float gh = sqrtf(w), gt = sqrtf(1.0f - w);
                            chunk[k * 2]     = (int16_t)(chunk[k * 2] * gh + tail[k * 2] * gt);
                            chunk[k * 2 + 1] = (int16_t)(chunk[k * 2 + 1] * gh + tail[k * 2 + 1] * gt);
                        }
                        fseek(f, sf_seek_pos(&sfl, cur_ff), SEEK_SET);   // resume
                    }
                }
                sd_lock_give();
                if (got > 0) {
                    uint32_t w = dk.wpos % DK_RING_FRAMES;
                    uint32_t first = DK_RING_FRAMES - w;
                    if (first > got) first = got;
                    memcpy(dk.ring + w * 2, chunk, first * 4);
                    if (first < got) memcpy(dk.ring, chunk + first * 2, (got - first) * 4);
                    dk.wpos += got;
                }
                if (dk.loading && dk.wpos - dk.rpos_i >= DK_LOW_WATER) dk.loading = false;
                continue;              // keep filling without the delay below
            }
            if (dk.loading && !lp && ff >= dk.file_frames) dk.loading = false;

            // waveform thumbnail, one column per idle pass (ring is warm
            // when we get here): seek away for a 128-frame peek, restore
            // the stream position. ~144 passes at a tick apiece — the
            // sampler3 tiny-white scheme, built without a playback hiccup.
            if (dk.wf_state == 1 && dk.file_frames) {
                uint32_t p = (uint32_t)((uint64_t)dk.wf_col * dk.file_frames / DK_WF_W);
                uint32_t want = dk.file_frames - p;
                if (want > 128) want = 128;
                sd_lock_take();
                fseek(f, sf_seek_pos(&sfl, p), SEEK_SET);
                size_t got = want ? sampfile_read(f, &sfl, chunk, want) : 0;
                cur_ff = (uint32_t)-1;      // handle moved: force a seek next fill
                sd_lock_give();
                int peak = 0;
                for (size_t k = 0; k < got * 2; k++) {
                    int sv = chunk[k];
                    if (sv < 0) sv = -sv;
                    if (sv > peak) peak = sv;
                }
                dk.wf[dk.wf_col] = (uint8_t)(peak >> 7);
                if (++dk.wf_col >= DK_WF_W) dk.wf_state = 2;
            }
        }
        vTaskDelay(1);   // >=1 tick: pdMS_TO_TICKS(5)==0 at 100Hz = busy-spin
    }
    if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
    free(chunk);
    free(tail);
    s_alive = false;
    vTaskDelete(NULL);
}

// ---- UI-side controls -------------------------------------------------------
int deck_load_track(const char *name)
{
    dk.loop_active = false;
    dk.playing = false;
    dk.loading = true;
    dk.track_bpm = 0;
    dk.bpm_raw = 0;
    dk.feel = 1.0f;
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
    sample_resolve_aux(name, ".JSN", jp, sizeof(jp));
    cJSON *root = readJSONFileAsCJSON(jp);
    if (root) {
        cJSON *j;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "bpm")) && cJSON_IsNumber(j))
            dk.bpm_raw = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "grid")) && cJSON_IsNumber(j))
            dk.grid_offset = (uint32_t)j->valuedouble;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "dver")) && cJSON_IsNumber(j))
            dver = j->valueint;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "feel")) && cJSON_IsNumber(j)) {
            float f = (float)j->valuedouble;
            if (f == 0.5f || f == 1.0f || f == 2.0f) dk.feel = f;
        }
        dk.track_bpm = dk.bpm_raw * dk.feel;
        cJSON_Delete(root);
    }
    if (dk.an_state != DK_AN_RUNNING) dk.an_state = DK_AN_IDLE;
    // auto-analyze when enabled and the sidecar had no bpm OR carries a
    // pre-ladder (v1) result — the library upgrades itself one load at a
    // time; a current (dver >= 2) track never re-analyzes automatically
    dk_tl_update();                 // track-loop window follows bpm/grid/loop
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
    dk.loop_active = false;               // a restart always exits the loop
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
    // resume at the cursor's FILE position — rpos_i is a playback counter now
    // (the loop rework), and seeking to it jumped the playhead forward by the
    // counter/file skew (Arlo, bench: "the playhead jumps forward substantially")
    uint32_t to = dk_map(dk.rpos_i);
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
    dk.loop_active = false;               // a scrub exits the loop
    uint32_t beat_tf = (dk.track_bpm > 20.0f)
        ? (uint32_t)(60.0f * DK_RATE / dk.track_bpm)
        : DK_RATE;                              // no grid yet: 1 s steps
    // snap current position to the nearest grid beat, then step whole beats —
    // scrubbing during sync'd playback lands phase-true by construction
    int64_t rel = (int64_t)dk_map(dk.rpos_i) - (int64_t)dk.grid_offset;
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

// FEEL: harmonic tempo ambiguity is taste — a 140-detection that grooves at
// 70 syncs half-time by default once its sidecar carries "feel": 0.5.
// UI task only (writes the sidecar).
void deck_set_feel(float f)
{
    if (f != 0.5f && f != 2.0f) f = 1.0f;
    dk.feel = f;
    if (dk.bpm_raw > 0) dk.track_bpm = dk.bpm_raw * f;
    dk_tl_update();                 // beats changed -> the wrap point moves
    if (!dk.track[0]) return;
    char jp[64];
    sample_resolve_aux(dk.track, ".JSN", jp, sizeof(jp));
    cJSON *root = readJSONFileAsCJSON(jp);
    if (!root) root = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(root, "feel");
    cJSON_AddNumberToObject(root, "feel", f);
    char *js = cJSON_Print(root);
    cJSON_Delete(root);
    if (js) { writeJSONFile(jp, js); free(js); }
}

// Hard-snap the playback phase so the track grid aligns to the external clock
// NOW, instead of waiting for the slow loop to pull in. Snaps to the current
// nudge offset target (keeps phase_offset). Called from the audio task on a TR2
// long-hold — only float math + a seek request, so it's safe there.
void deck_sync_now(void)
{
    if (!dk.track[0] || !dk.file_frames) return;
    if (!dk.sync || !dk.ci.clk.locked || dk.ci.clk.period == 0 || dk.track_bpm <= 20.0f) return;
    uint32_t beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
    float seg_tf = (float)beat_tf / DK_PPB_EFF() * dk.speed_mult;   // frames/pulse
    if (seg_tf < 1.0f) return;
    float p_ext = (float)dk.ci.clk.since / (float)dk.ci.clk.period;
    if (p_ext > 1.0f) p_ext = 1.0f;
    int64_t rel = (int64_t)dk_map(dk.rpos_i) - (int64_t)dk.grid_offset;
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

// ---- KO-II buffer LOOP (convergence S6-S7) -----------------------------------
// Toggle is audio-task-safe (float math + flags only). ENGAGE is seamless: the
// window anchors on the grid beat NEAREST rpos — ring-resident thanks to the
// S5 reader lead cap (guarded: snaps forward whole beats if not) — and the
// first audible effect is the first wrap. RELEASE: freeze = just drop the
// flag (seamless by residency); keeps-running = seek to the phantom position
// playback would have reached (engage_ff + loop_adv), a brief duck — while
// FREEZE (the seamless option) simply rebases the mapping and plays on.
// Arlo's ladder, in QUARTER-BEATS: 1/4 beat .. 256 beats. The window streams,
// so length is bounded by the TRACK, not by the ring.
static const int dk_loop_q[11] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
#define DK_LOOP_STEPS 11
static int s_loop_len_idx = 4;              // 4 beats (16 quarters); per session

// a window must hold a WHOLE number of clock pulses or every wrap shifts the
// phase and the PLL spends the loop fighting it. One pulse = 4/ppb quarters.
static int dk_min_q(void)
{
    float ppb = DK_PPB_EFF();
    int q = (ppb > 0) ? (int)(4.0f / ppb + 0.999f) : 4;
    return q < 1 ? 1 : q;
}
static uint32_t dk_len_for(int idx, uint32_t beat_tf)
{
    if (idx < 0 || idx >= DK_LOOP_STEPS || !beat_tf) return 0;
    return (uint32_t)((uint64_t)dk_loop_q[idx] * beat_tf / 4);
}

static void deck_loop_toggle(void)
{
    if (dk.loop_active) {
        // RELEASE. Rebase the mapping to LINEAR at the cursor's current file
        // frame. Everything the reader already buffered from here to the next
        // wrap maps identically under both mappings, so it stays valid — the
        // ring keeps playing and there is no duck.
        uint32_t p = dk.rpos_i;
        uint32_t off = (p - dk.map_p0) % dk.loop_len_fr;
        uint32_t ff = dk.loop_start + off;
        uint32_t valid_to = p + (dk.loop_len_fr - off);   // the next seam
        dk.map_p0 = p;
        dk.map_f0 = ff;
        dk.rm_at = 0;
        dk.loop_active = false;            // mapping written BEFORE the flag
        if (dk.wpos > valid_to) dk.wpos = valid_to;
        s_pk6 = s_pk7 = -1;                // knobs stay put until they MOVE
        if (!dk.loop_freeze) {
            // keeps-running: jump to where playback WOULD be by now
            uint64_t ph = (uint64_t)dk.engage_ff + dk.loop_adv;
            if (dk.file_frames && ph >= dk.file_frames) {
                if (dk.loop && dk.file_frames > dk.grid_offset)
                    ph = dk.grid_offset +
                         (ph - dk.grid_offset) % (dk.file_frames - dk.grid_offset);
                else ph = dk.file_frames - 1;
            }
            dk.loading = true;
            dk.phase_int = 0;
            dk.sync_slew = 0;
            dk.seek_to = (uint32_t)ph;
            dk.seek_req = true;
        }
        return;
    }
    // ENGAGE. Gates: mid-air states and gridless tracks can't loop.
    if (!dk.playing || dk.loading || dk.seek_req || dk.track_bpm <= 20.0f ||
        !dk.file_frames) return;
    uint32_t beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
    if (!beat_tf) return;
    uint32_t ff = dk_map(dk.rpos_i);
    if (ff >= dk.file_frames) return;
    // anchor on the grid beat AT OR BEFORE the cursor (floor, not nearest): the
    // window must CONTAIN the cursor or the mapping can't stay continuous
    int64_t rel = (int64_t)ff - (int64_t)dk.grid_offset;
    int64_t idx = (rel >= 0) ? rel / (int64_t)beat_tf
                             : -(((-rel) + beat_tf - 1) / (int64_t)beat_tf);
    int64_t st = (int64_t)dk.grid_offset + idx * (int64_t)beat_tf;
    if (st < 0) st = 0;
    if (st > (int64_t)ff) st -= beat_tf;          // paranoia: never ahead of the cursor
    if (st < 0) st = 0;
    int minq = dk_min_q();
    while (s_loop_len_idx < DK_LOOP_STEPS - 1 && dk_loop_q[s_loop_len_idx] < minq)
        s_loop_len_idx++;                       // never below one clock pulse
    uint32_t len = dk_len_for(s_loop_len_idx, beat_tf);
    // the window must fit the TRACK — the ring is no longer the ceiling
    while (len > 0 && st + (int64_t)len > (int64_t)dk.file_frames &&
           s_loop_len_idx > 0 && dk_loop_q[s_loop_len_idx - 1] >= minq) {
        s_loop_len_idx--;
        len = dk_len_for(s_loop_len_idx, beat_tf);
    }
    if (!len || st + (int64_t)len > (int64_t)dk.file_frames) return;
    int lb = dk_loop_q[s_loop_len_idx];         // quarter-beats (UI formats it)
    // The window may be SHORTER than a beat, in which case the cursor's offset
    // into the beat can exceed the whole window. Slide the start forward by
    // whole windows so the cursor lands INSIDE it (still phase-true: the start
    // stays beat + k*window, and a window is a whole number of clock pulses).
    // Getting this wrong put wpos BEHIND rpos, the unsigned lead underflowed,
    // and the reader stopped filling forever — a permanent starve (bench).
    if ((uint32_t)(ff - (uint32_t)st) >= len)
        st += (int64_t)(((uint32_t)(ff - (uint32_t)st)) / len) * (int64_t)len;
    if (st + (int64_t)len > (int64_t)dk.file_frames) return;
    uint32_t off = ff - (uint32_t)st;             // cursor's offset into the window
    dk.loop_start = (uint32_t)st;
    dk.loop_len_fr = len;
    dk.loop_len_beats = lb;                     // in QUARTER-beats
    dk.loop_adv = 0;
    dk.engage_ff = ff;
    dk.map_p0 = dk.rpos_i - off;                  // keeps map(rpos) == ff
    dk.rm_at = 0;                                 // no stale scheduled window
    dk.loop_active = true;                        // set LAST (write ordering)
    // the read-ahead stays valid up to the first seam; the reader wraps there
    uint32_t valid_to = dk.map_p0 + len;
    if (dk.wpos > valid_to) dk.wpos = valid_to;
}

// window move / length change while looping. The new mapping takes effect AT
// THE READER'S FRONTIER — everything already buffered plays out first and the
// reader simply starts filling the new window from wpos onward.
//
// The earlier version truncated the read-ahead to force the move through
// faster. That STARVED the ring, and a starve does not merely drop audio: the
// engine holds the cursor while real time runs on, so the deck slips against
// the clock and the PLL spends the next seconds hauling it back (Arlo: "moving
// the start point gets out of phase with the pll"). Latency is the honest cost
// here — the loop stays phase-true and nothing drops.
// the window in force for CONTROL purposes: pending if a move is scheduled.
// Comparing knobs against the COMMITTED window instead made CV6 reschedule the
// same move every few blocks, pushing the commit point forward forever — so the
// loop start never actually changed (Arlo: "cv6 seems to do a lot of nothing").
static inline uint32_t dk_live_start(void){ return dk.rm_at ? dk.rm_start : dk.loop_start; }
static inline uint32_t dk_live_len(void)  { return dk.rm_at ? dk.rm_len   : dk.loop_len_fr; }

static void deck_loop_remap(uint32_t new_start, uint32_t new_len)
{
    if (!dk.loop_active || !new_len) return;
    if (new_start + new_len > dk.file_frames) return;
    uint32_t at = dk.wpos;                        // first frame the reader writes
    uint32_t off = (at - dk.map_p0) % dk.loop_len_fr;
    if (off >= new_len) off %= new_len;           // shrank under the cursor
    dk.rm_start = new_start;
    dk.rm_len = new_len;
    dk.rm_p0 = at - off;                          // phase carries across the switch
    dk.rm_at = at ? at : 1;                       // 0 means "none"
}

// RESYNC — the both-trig gesture (Arlo: "long press both tr1 tr2 forces resync
// of the pll with beat landing on release"). Hold TR1+TR2, and on RELEASE the
// deck's beat lands exactly THERE. Two things have to happen together or it
// does not stick:
//
//   1. the TRACK moves to the nearest beat boundary — via the capped rate bend
//      (sync_slew), never a seek, so nothing drops and nothing clicks;
//   2. the PLL's LOCK POINT moves with it (phase_offset := the clock phase at
//      the release instant, plus the output-chain lead). Without this the loop
//      would spend the next few seconds hauling the beat back to wherever the
//      grid stamp thought it was, and the gesture would silently undo itself.
//
// So this is also the manual NUDGE: a bad grid stamp is corrected by feel, on
// the beat, with no knob to steal. Audio-task safe (float math + a slew).
void deck_resync_now(void)
{
    if (!dk.playing || !dk.track[0] || !dk.file_frames) return;
    if (dk.track_bpm <= 20.0f) return;
    uint32_t beat_tf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
    if (!beat_tf) return;

    // 1. how far is the cursor from a beat? Go to the NEARER one, so the pull
    //    is never more than half a beat and the bend stays inaudible.
    int64_t rel = (int64_t)dk_map(dk.rpos_i) - (int64_t)dk.grid_offset;
    int64_t off = rel % (int64_t)beat_tf;
    if (off < 0) off += beat_tf;
    float shift = (off <= beat_tf / 2) ? -(float)off        // retard to this beat
                                       : (float)(beat_tf - off);  // advance to the next
    dk.sync_slew = shift;

    // 2. the release instant IS the beat: park the lock point on the clock
    //    phase it happened at (the lead keeps the AUDIBLE beat where the ear
    //    expects it — the output chain lands beats ~13 ms late).
    if (dk.sync && dk.ci.clk.locked && dk.ci.clk.period > 0) {
        float p_ext = (float)dk.ci.clk.since / (float)dk.ci.clk.period;
        if (p_ext > 1.0f) p_ext = 1.0f;
        float lead = DK_LAG_LEAD_FR / (float)dk.ci.clk.period;
        float po = p_ext + lead;
        po -= floorf(po);                      // wrap to [0,1)
        dk.phase_offset = po;
    }
    dk.phase_int = 0.0f;                       // start the new lock clean
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
    dk.feel = 1.0f;
    dk.clk_scale = 1.0f;
    clockin_reset(&dk.ci, DK_PPB_EFF());
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

    // transport gates — the unified TR grammar (convergence S6, shared
    // trig_gate.h, same contract as the tracker): TR1 tap = play/pause, TR1
    // hold-release = restart at the downbeat; TR2 press = LOOP toggle (engage
    // on PRESS, beat-true), TR2 held past 0.6 s = momentary loop (the long
    // release toggles back out). Flags only; the reader task seeks.
    // MIGRATION: TR1 tap no longer restarts (that moved to the hold), TR2 tap
    // no longer play/pauses (that moved to TR1). deck_sync_now() is UNBOUND —
    // shelved 2026-07-13; the PLL + NUDGE cover the need. Kept in code as the
    // grave marker for a future binding.
    static trig_gate_t tg1, tg2;
    const int nfr = MACHINE_BLOCK / 2;
    bool d1 = !(io->trig_level & 1), d2 = !(io->trig_level & 2);
    tg_event_t e1 = trig_gate_step(&tg1, d1, nfr);
    tg_event_t e2 = trig_gate_step(&tg2, d2, nfr);
    // BOTH-TRIG RESYNC: arms at 0.35 s (ahead of either gate's own 0.6 s hold),
    // fires on release. While it owns the trigs, their individual events are
    // swallowed — otherwise the same gesture would also stop the deck and flip
    // the loop on its way past.
    static trig_combo_t tc;
    tc_event_t ec = trig_combo_step(&tc, d1, d2, nfr);
    if (ec == TC_FIRE) deck_resync_now();
    dk.resync_armed = (ec == TC_ARMED) ? true : (trig_combo_busy(&tc) ? dk.resync_armed : false);
    if (!trig_combo_busy(&tc)) {
        if (e1 == TG_REL_SHORT) deck_toggle_play();
        else if (e1 == TG_REL_LONG) deck_restart();
        if (e2 == TG_PRESS || e2 == TG_REL_LONG) deck_loop_toggle();
    }

    // While LOOPING the two good knobs become the loop's: CV7 = length ladder
    // (1..16 beats), CV6 = whole-window start moves. The filter/speed reads
    // simply stop updating (frozen values); release restore is pop-free by
    // construction (flt_f slew + rate_sm glide toward the physical positions).
    // Relative deltas from engage-captured references; a move must persist a
    // few blocks so a WiFi ADC spike can't jump the window (dualdeck lesson).
    static int s_cv6_ref = -1, s_cv7_ref = -1, s_mv6 = 0, s_mv7 = 0;
    static int s_c6_last = -1;    // knob6 position the last window move was made AT
    if (dk.loop_active) {
        if (s_cv6_ref == -1) { s_cv6_ref = io->cv[5]; s_mv6 = 0; }   // arm: dead until moved
        if (s_cv7_ref == -1) { s_cv7_ref = io->cv[6]; s_mv7 = 0; }   // arm: dead until moved
        uint32_t beat_tf_lp = (dk.track_bpm > 20.0f)
            ? (uint32_t)(60.0f * DK_RATE / dk.track_bpm) : 0;
        // CV7 = LENGTH ladder, ABSOLUTE: the knob's position IS the rung, so the
        // whole ladder is one sweep and a small turn moves a rung (the old
        // relative deltas needed 512 counts per step — "real slow", Arlo).
        // Grab-then-track: the knob is dead until it MOVES past the deadband,
        // so engaging a loop can't slam the length to wherever the knob sits.
        if (beat_tf_lp) {
            int c7 = io->cv[6];
            if (s_cv7_ref >= 0 &&
                (c7 - s_cv7_ref > DK_PICKUP || s_cv7_ref - c7 > DK_PICKUP))
                s_cv7_ref = -2;                  // grabbed: knob is live now
            if (s_cv7_ref == -2) {
                int ni = c7 * DK_LOOP_STEPS / 4096;
                if (ni < 0) ni = 0;
                if (ni > DK_LOOP_STEPS - 1) ni = DK_LOOP_STEPS - 1;
                int minq = dk_min_q();
                while (ni < DK_LOOP_STEPS - 1 && dk_loop_q[ni] < minq) ni++;
                // hysteresis: only move when the knob is clear of the boundary,
                // so ADC noise can't flap the length
                int lo = ni * 4096 / DK_LOOP_STEPS, hi = (ni + 1) * 4096 / DK_LOOP_STEPS;
                bool solid = (c7 > lo + 40) && (c7 < hi - 40);
                if (ni != s_loop_len_idx && solid && ++s_mv7 >= 3) {
                    s_mv7 = 0;
                    uint32_t nl = dk_len_for(ni, beat_tf_lp);
                    uint32_t cs = dk_live_start();
                    if (nl && cs + nl <= dk.file_frames) {
                        s_loop_len_idx = ni;
                        dk.loop_len_beats = dk_loop_q[ni];
                        deck_loop_remap(cs, nl);
                    }
                } else if (ni == s_loop_len_idx) s_mv7 = 0;
            }
        }
        // CV6 = WINDOW position, ABSOLUTE across the WHOLE TRACK (Arlo: "cv6
        // loop start seems to not travel very far" — it used to step one window
        // per 128 counts, so a full sweep crossed only a fraction of a long
        // track). The knob now maps to a window INDEX from the downbeat, so one
        // sweep spans the track and the landing stays phase-true (a whole
        // number of windows from the grid = a whole number of clock pulses).
        // Grab-then-track, so engaging a loop can't fling the window.
        // CV6 acts ONLY when the KNOB ITSELF moves (s_c6_last). Its mapping is
        // absolute in WINDOWS, so re-evaluating it on a CV7 length change
        // re-quantizes the same knob position onto a coarser grid and drags the
        // start backwards (Arlo: "sometimes turning up cv7 expands the loop
        // backwards by moving the start point back"). Growing a loop must keep
        // the start where it is and extend the END — which is exactly what CV7
        // does on its own, as long as CV6 stays out of it.
        {
            int c6 = io->cv[5];
            if (s_cv6_ref >= 0 &&
                (c6 - s_cv6_ref > DK_PICKUP || s_cv6_ref - c6 > DK_PICKUP))
                s_cv6_ref = -2;                          // grabbed
            uint32_t llen = dk_live_len();
            int moved = (s_c6_last < 0) || (c6 - s_c6_last > 24) || (s_c6_last - c6 > 24);
            if (s_cv6_ref == -2 && llen && moved) {
                uint32_t span = (dk.file_frames > dk.grid_offset)
                              ? dk.file_frames - dk.grid_offset : 0;
                uint32_t nwin = span / llen;              // windows in the track
                if (nwin) {
                    uint32_t idx = (uint32_t)((uint64_t)c6 * nwin / 4096);
                    if (idx >= nwin) idx = nwin - 1;
                    uint32_t ns = dk.grid_offset + idx * llen;
                    if (ns != dk_live_start() && ++s_mv6 >= 3) {
                        s_mv6 = 0;
                        s_c6_last = c6;                  // this knob position is spent
                        if (ns + llen <= dk.file_frames) deck_loop_remap(ns, llen);
                    } else if (ns == dk_live_start()) { s_mv6 = 0; s_c6_last = c6; }
                }
            }
        }
    } else { s_cv6_ref = -1; s_cv7_ref = -1; s_c6_last = -1; }   // re-arm for the next engage

    int mode = dk.flt_mode;                  // frozen while looping
    const float q = 0.9f;
    if (!dk.loop_active) {
        // PASS-THROUGH pickup: a knob the loop borrowed stays inert until it
        // returns to the value the engine is still using — then it takes over
        // seamlessly (see DK_PASSTOL). Nothing ever jumps.
        int c6 = io->cv[5], c7 = io->cv[6];
        if (s_pk7 != -2) {
            int d = c7 - (int)dk.pitch_cv;
            if (d < 0) d = -d;
            if (d <= DK_PASSTOL) s_pk7 = -2;     // knob has caught up: it is live
        }
        if (s_pk6 != -2) {
            int d = c6 - (int)dk.filt_cv;
            if (d < 0) d = -d;
            if (d <= DK_PASSTOL) s_pk6 = -2;
        }

        if (s_pk7 == -2) dk.pitch_cv = c7;   // knob7 = speed / free-run rate
        if (s_pk6 != -2) goto filter_done;   // knob6 still frozen: keep the
                                             // filter exactly where it was

        // DJ filter from knob6: centre dead zone = bypass; left half sweeps a
        // low-pass down (12 kHz -> 80 Hz), right half a high-pass up (30 Hz ->
        // 6 kHz). Exponential sweeps; coefficient slewed per block (no zipper).
        int fcv = c6;
        dk.filt_cv = fcv;
        mode = 0;
        float fc = 0;
        if (fcv < 2048 - 150) {              // LP zone
            mode = 1;
            float t = (float)fcv / (2048.0f - 150.0f);      // 1..0 as knob goes left
            fc = 80.0f * powf(150.0f, t);                   // 80 Hz .. 12 kHz
        } else if (fcv > 2048 + 150) {       // HP zone
            mode = 2;
            float t = (float)(fcv - 2048 - 150) / (4095.0f - 2048.0f - 150.0f);
            fc = 30.0f * powf(200.0f, t);                   // 30 Hz .. 6 kHz
        }
        dk.flt_mode = mode;
        // 1.2 is not a tidy-up candidate: 12 kHz gives an unclamped ~1.51, so
        // this clamp IS the top of the LP sweep
        float f_target = mode ? svf_coef(fc, (float)DK_RATE, 1.2f) : 0;
        dk.flt_f += 0.2f * (f_target - dk.flt_f);
    }
filter_done:;                // mild resonance, DJ-ish

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
        if (dk.ci.clk.locked && dk.ci.clk.period > 0 && beat_tf > 0) {
            float ppb = DK_PPB_EFF();
            // pulse-level phase lock (works for any mult/div): compare phase
            // within one external pulse against the track's matching segment
            float seg_tf = (float)beat_tf / ppb * m;           // track frames per pulse
            float base = seg_tf / (float)dk.ci.clk.period;        // nominal rate
            float p_ext = (float)dk.ci.clk.since / (float)dk.ci.clk.period;
            if (p_ext > 1.0f) p_ext = 1.0f;
            // FILE position, not the playback counter: the counter carries a
            // seek/loop skew, and locking against it puts the beat grid on a
            // shifted reference (E still reads 0 — a silent wrongness)
            float p_trk = fmodf((float)((int64_t)dk_map(dk.rpos_i) - (int64_t)dk.grid_offset),
                                seg_tf) / seg_tf;
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
            float lead = DK_LAG_LEAD_FR / (float)dk.ci.clk.period;
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
        // wpos is the reader's frontier in PLAYBACK space; the file end only
        // bounds playback when we're NOT looping (a loop never reaches it)
        bool can_play = dk.playing && !dk.loading && dk.rpos_i + 1 < dk.wpos;
        uint32_t ff_now = dk_map(dk.rpos_i);
        // mid-track mute with the reader as the limiter = ring underrun;
        // counted per block into /status so click reports are attributable
        if (!can_play && dk.playing && !dk.loading && !dk.seek_req &&
            dk.file_frames && (dk.loop_active || ff_now + 1 < dk.file_frames))
            starved = true;
        // declick both edges: gain ramps in on resume; on mute the last
        // sample decays out instead of stepping to zero (each scrub detent
        // used to click — "beeps")
        float gt = can_play ? 1.0f : 0.0f;
        dk.out_gain += (gt - dk.out_gain) * 0.015f;
        if (!can_play) {
            // EOF only exists when NOTHING is windowing playback: a track loop
            // now wraps inside the mapping (phase-exact, no seek, no PLL reset
            // — the old restart-on-EOF is what "re-detected tempo every cycle")
            if (dk.playing && !dk.loading && !dk.loop_active && !dk.tl_len &&
                dk.file_frames && ff_now + 1 >= dk.file_frames && !dk.seek_req) {
                dk.playing = false;
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

        if (mode) {                       // Chamberlin SVF per channel (util/svf.h)
            float lo, hi;
            svf_step(&dk.flt_l, l, dk.flt_f, q, &lo, NULL, &hi);
            l = (mode == 1) ? lo : hi;
            svf_step(&dk.flt_r, r, dk.flt_f, q, &lo, NULL, &hi);
            r = (mode == 1) ? lo : hi;
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
        } else {
            svf_park(&dk.flt_l, l);       // park state at the signal: no thump
            svf_park(&dk.flt_r, r);       // when the filter re-engages
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
        while (dk.rpos_f >= 1.0) {
            dk.rpos_f -= 1.0;
            dk.rpos_i++;                          // playback counter: NEVER wraps
            if (dk.loop_active) dk.loop_adv++;    // feeds the release phantom
        }
    }
    if (starved) dk.dbg_starve++;
    // the scheduled window becomes the live one once the cursor reaches it
    if (dk.loop_active && dk.rm_at && dk.rpos_i >= dk.rm_at) {
        dk.loop_start = dk.rm_start;
        dk.loop_len_fr = dk.rm_len;
        dk.map_p0 = dk.rm_p0;
        dk.rm_at = 0;
    }
    dk.ui_fpos = dk_map(dk.rpos_i);      // the UI reads FILE position, not counters
    dk.ui_lstart = dk_live_start();      // ...and the window it should DRAW
    dk.ui_llen = dk_live_len();
    // keep the track-loop window in step with Setup/analysis (cheap, no I/O)
    {
        static uint32_t s_tl_sig = 0;
        uint32_t sig = (dk.loop ? 1u : 0u) ^ (dk.grid_offset << 1) ^
                       ((uint32_t)(dk.track_bpm * 100) << 3) ^ dk.file_frames;
        if (sig != s_tl_sig) { s_tl_sig = sig; dk_tl_update(); }
    }

    // clock conditioning lives in the shared front-end now (clockin_t,
    // components/machine/clock.{h,c} — the code this replaced is what it was
    // extracted from). set_ppb every block keeps the pulse-rate sanity gates
    // scaled and, ON an actual mult/div change, drops the lock for a clean
    // 2-pulse relock instead of letting the guards defend the stale period.
    clockin_set_ppb(&dk.ci, DK_PPB_RAW());   // gates take the RAW setting
    clockin_block(&dk.ci, io->cv[dk.clk_src & 7], frames);
}

// ---- preset -------------------------------------------------------------------
static cJSON *deck_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "track", dk.track);
    cJSON_AddBoolToObject(o, "sync", dk.sync);
    cJSON_AddBoolToObject(o, "loop", dk.loop);
    cJSON_AddBoolToObject(o, "auto_an", dk.auto_an);
    cJSON_AddBoolToObject(o, "loop_freeze", dk.loop_freeze);
    cJSON_AddNumberToObject(o, "clk_src", dk.clk_src);
    cJSON_AddNumberToObject(o, "ppb", dk.ppb_idx);
    cJSON_AddNumberToObject(o, "clkx", (double)dk.clk_scale);
    cJSON_AddNumberToObject(o, "llenq", dk_loop_q[s_loop_len_idx]);   // QUARTER-beats
    return o;
}

static void deck_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sync"))) dk.sync = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "loop"))) dk.loop = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "loop_freeze"))) dk.loop_freeze = cJSON_IsTrue(j);
    // must land before the track restore below so it gates the boot-time load
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "auto_an"))) dk.auto_an = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j)) dk.clk_src = j->valueint & 7;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ppb")) && cJSON_IsNumber(j)) {
        dk.ppb_idx = j->valueint;
        if (dk.ppb_idx < 0) dk.ppb_idx = 0;
        if (dk.ppb_idx > 5) dk.ppb_idx = 5;
    }
    // only (re)load when the track actually changes — a remote "Apply" that
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clkx")) && cJSON_IsNumber(j)) {
        float cs = (float)j->valuedouble;
        dk.clk_scale = (cs == 0.5f || cs == 2.0f) ? cs : 1.0f;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "llenq")) && cJSON_IsNumber(j)) {
        int want = j->valueint;                  // quarter-beats
        for (int k = 0; k < DK_LOOP_STEPS; k++)
            if (dk_loop_q[k] == want) {
                s_loop_len_idx = k;
                if (dk.loop_active && dk.track_bpm > 20.0f) {
                    uint32_t btf = (uint32_t)(60.0f * DK_RATE / dk.track_bpm);
                    uint32_t nl = dk_len_for(k, btf);
                    if (nl && dk.loop_start + nl <= dk.file_frames) {
                        dk.loop_len_beats = want;
                        deck_loop_remap(dk.loop_start, nl);
                    }
                }
                break;
            }
    }
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
