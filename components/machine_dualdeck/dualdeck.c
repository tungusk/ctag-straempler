// Dual-deck engine — two dk-style streaming decks off ONE reader task, both
// phase-locked to the shared conditioned clock, blended by an equal-power
// crossfade with takeover automation, summed through a master DJ filter.
// Transport is BAR-quantized (Arlo: entries and exits are phrase-aligned).
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
#include "dualdeck_priv.h"

static const char *TAG = "DDECK";

dd_state_t dd;
const float dd_ppb[6] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
const char *const dd_ppb_names[6] = {"1 per 4 beats", "1 per 2 beats", "1 per beat",
                                     "2 per beat", "4 per beat", "8 per beat"};

#define DD_LAG_LEAD_FR (0.0131f * 44100.0f)   // output-chain lead (deck-measured)
#define DD_SLEW_GAIN 0.0006f  // drains the resync shift over ~1 s
#define DD_SLEW_MAX  0.15f    // max +/-15% rate bend — a bend, not a jump
#define DD_XF_GRAB 220        // knob counts of movement that grab the fader back
#define DD_XFADE   256        // ~5.8 ms seam fade — a click-killer, not a blur
// Read-ahead while looping: this IS the latency of a window change, so it wants
// to be small — but it must stay ABOVE DD_LOW_WATER or the reader parks below
// the level the engine calls "buffered" and the ring starves continuously
// (bench-caught on the deck: a 0.25 s cap -> 32k starve blocks in 20 s).
#define DD_LOOP_LEAD (DD_RATE / 2)

// loop-length ladder in QUARTER-beats: 1/4 beat .. 256 beats (Arlo's, from the
// deck). The window STREAMS, so length is bounded by the track, not the ring.
const int dd_loop_q[DD_LOOP_STEPS] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

void dd_fmt_beats(int q, char *out, int n)
{
    if (q == 1)      snprintf(out, n, "1/4");
    else if (q == 2) snprintf(out, n, "1/2");
    else             snprintf(out, n, "%d", q / 4);
}

static volatile bool s_run = false, s_alive = false;

// playback counter -> FILE frame. The reader owns this mapping; everyone else
// (engine, UI) goes through it. A loop is a mapping, not a cursor wrap.
static inline uint32_t dd_map(dd_deck_t *v, uint32_t p)
{
    if (v->loop_active && v->rm_at && p >= v->rm_at && v->rm_len)
        return v->rm_start + ((p - v->rm_p0) % v->rm_len);      // scheduled window
    if (v->loop_active && v->loop_len_fr)
        return v->loop_start + ((p - v->map_p0) % v->loop_len_fr);
    if (v->tl_len) {                       // track loop: wrap at the last beat
        int64_t rel = (int64_t)v->map_f0 + (int64_t)(p - v->map_p0) - (int64_t)v->tl_start;
        if (rel >= 0) return v->tl_start + (uint32_t)(rel % (int64_t)v->tl_len);
    }
    return v->map_f0 + (p - v->map_p0);
}

// the active streaming window (KO-II loop, else track loop). false = linear.
static inline bool dd_window(dd_deck_t *v, uint32_t *st, uint32_t *len)
{
    if (v->loop_active && v->rm_at && v->wpos >= v->rm_at && v->rm_len) {
        *st = v->rm_start; *len = v->rm_len; return true;
    }
    if (v->loop_active && v->loop_len_fr) { *st = v->loop_start; *len = v->loop_len_fr; return true; }
    if (v->tl_len && dd_map(v, v->rpos_i) >= v->tl_start) { *st = v->tl_start; *len = v->tl_len; return true; }
    return false;
}

// (re)compute the track-loop window: whole beats from the downbeat. An
// unstamped track still wraps (at the raw file end) — decks loop by design.
static void dd_tl_update(dd_deck_t *v)
{
    if (!v->file_frames || v->grid_offset >= v->file_frames) { v->tl_len = 0; return; }
    uint32_t span = v->file_frames - v->grid_offset;
    if (v->track_bpm > 20.0f) {
        uint32_t beat_tf = (uint32_t)(60.0f * DD_RATE / v->track_bpm);
        if (beat_tf) {
            uint32_t n = span / beat_tf;                // whole beats only
            if (n) span = n * beat_tf;                  // trim the partial tail
        }
    }
    v->tl_start = v->grid_offset;
    v->tl_len = span;
}

// ---- reader task (one task, both decks — sampler3's shared-reader pattern) ----
static void reader_serve(dd_deck_t *v, FILE **fp, char *cur, uint32_t *cur_ff,
                         int16_t *chunk, int16_t *tail)
{
    if (v->track_req) {
        v->track_req = false;
        if (*fp) { sd_lock_take(); fclose(*fp); sd_lock_give(); *fp = NULL; }
        strlcpy(cur, v->pending, DD_NAME_LEN);
        if (cur[0]) {
            char path[64];
            sample_resolve(cur, path, sizeof(path));
            sd_lock_take();
            *fp = fopen(path, "rb");
            if (*fp && sampfile_probe(*fp, &v->sf) != 0) {
                ESP_LOGE(TAG, "%s: %s", path, v->sf.why);
                fclose(*fp); *fp = NULL;
            }
            if (*fp) v->file_frames = v->sf.frames;
            sd_lock_give();
            if (!*fp) { ESP_LOGE(TAG, "open %s failed", path); v->file_frames = 0; }
            v->wpos = 0; v->rpos_i = 0; v->rpos_f = 0;
            v->map_p0 = 0; v->map_f0 = 0;
            *cur_ff = (uint32_t)-1;
            v->wf_state = (*fp && v->file_frames) ? 1 : 0;
            v->wf_col = 0;
            dd_tl_update(v);
            // park at the cue: pre-fill from the grid downbeat so a quantized
            // start needs no SD round-trip
            v->seek_to = (v->grid_offset < v->file_frames) ? v->grid_offset : 0;
            v->seek_req = true;
        }
    }
    if (*fp && v->seek_req) {
        vTaskDelay(1);                    // settle: engine has parked on `loading`
        v->seek_req = false;
        uint32_t to = v->seek_to;
        if (to >= v->file_frames) to = 0;
        // counters stay MONOTONIC (ring slots depend on them) — a seek rewrites
        // the MAPPING instead of rewinding them
        uint32_t base = v->wpos + DD_RING_FRAMES;   // fresh, non-aliasing slots
        v->map_p0 = base;
        v->map_f0 = to;
        v->rpos_i = base;                 // reader is the ONLY seek-writer of rpos
        v->rpos_f = 0;
        v->wpos = base;
        *cur_ff = (uint32_t)-1;           // force a file seek on the next fill
    }
    if (*fp && !v->track_req && !v->seek_req) {
        uint32_t win_st = 0, win_len = 0;
        bool lp = dd_window(v, &win_st, &win_len);
        // SIGNED: if the frontier ever lands behind the cursor, an unsigned lead
        // underflows to ~4e9, the reader thinks it is far ahead and never fills
        // again — a silent permanent starve. Heal it. (deck bench lesson)
        int32_t slead = (int32_t)(v->wpos - v->rpos_i);
        if (slead < 0) {
            ESP_LOGW(TAG, "ring lead negative (%ld) — resyncing", (long)slead);
            v->wpos = v->rpos_i;
            *cur_ff = (uint32_t)-1;
            slead = 0;
        }
        uint32_t lead = (uint32_t)slead;
        // a KO-II loop keeps a SHORT lead (it is the latency of a window move);
        // otherwise read far ahead as usual
        uint32_t lead_cap = (v->loop_active && v->loop_len_fr)
                                ? DD_LOOP_LEAD
                                : (DD_RING_FRAMES - DD_RATE - 4096);
        uint32_t ff = dd_map(v, v->wpos);
        bool have_room = lp ? (lead < lead_cap)
                            : (ff < v->file_frames && lead < lead_cap);
        if (have_room && ff < v->file_frames) {
            // never read past the window end (loop) or the file end (linear)
            uint32_t limit = lp ? (win_st + win_len - ff) : (v->file_frames - ff);
            uint32_t want = limit < 4096 ? limit : 4096;
            bool seam = lp && ff == win_st && v->wpos > v->map_p0;
            sd_lock_take();
            if (*cur_ff != ff) { fseek(*fp, sf_seek_pos(&v->sf, ff), SEEK_SET); *cur_ff = ff; }
            size_t got = sampfile_read(*fp, &v->sf, chunk, want);
            *cur_ff += got;
            // SEAM: blend the incoming head against the tail CONTINUING past the
            // window end — phase-exact, so the cycle keeps its full length (a
            // fade built from the head itself would shorten every cycle and the
            // PLL would spend the loop fighting it)
            if (seam && got > 0) {
                uint32_t lend = win_st + win_len;
                uint32_t nx = got < DD_XFADE ? (uint32_t)got : DD_XFADE;
                if (lend + nx <= v->file_frames) {
                    fseek(*fp, sf_seek_pos(&v->sf, lend), SEEK_SET);
                    size_t tg = sampfile_read(*fp, &v->sf, tail, nx);
                    for (size_t k = 0; k < tg; k++) {
                        float w = (float)k / (float)DD_XFADE;      // 0 -> 1
                        float gh = sqrtf(w), gt = sqrtf(1.0f - w); // equal power
                        chunk[k * 2]     = (int16_t)(chunk[k * 2]     * gh + tail[k * 2]     * gt);
                        chunk[k * 2 + 1] = (int16_t)(chunk[k * 2 + 1] * gh + tail[k * 2 + 1] * gt);
                    }
                    fseek(*fp, sf_seek_pos(&v->sf, *cur_ff), SEEK_SET);   // resume
                }
            }
            sd_lock_give();
            if (got > 0) {
                uint32_t w = v->wpos % DD_RING_FRAMES;
                uint32_t first = DD_RING_FRAMES - w;
                if (first > got) first = got;
                memcpy(v->ring + w * 2, chunk, first * 4);
                if (first < got) memcpy(v->ring, chunk + first * 2, (got - first) * 4);
                v->wpos += got;
            }
            if (v->loading && v->wpos - v->rpos_i >= DD_LOW_WATER) v->loading = false;
            return;                        // keep filling on the next pass
        }
        if (v->loading && !lp && ff >= v->file_frames) v->loading = false;

        // waveform thumbnail, one column per idle pass once the ring is warm
        if (v->wf_state == 1 && v->file_frames) {
            uint32_t p = (uint32_t)((uint64_t)v->wf_col * v->file_frames / DD_WF_W);
            uint32_t want = v->file_frames - p;
            if (want > 128) want = 128;
            sd_lock_take();
            fseek(*fp, sf_seek_pos(&v->sf, p), SEEK_SET);
            size_t got = want ? sampfile_read(*fp, &v->sf, chunk, want) : 0;
            *cur_ff = (uint32_t)-1;        // handle moved: force a seek next fill
            sd_lock_give();
            int peak = 0;
            for (size_t k = 0; k < got * 2; k++) {
                int sv = chunk[k];
                if (sv < 0) sv = -sv;
                if (sv > peak) peak = sv;
            }
            v->wf[v->wf_col] = (uint8_t)(peak >> 7);
            if (++v->wf_col >= DD_WF_W) v->wf_state = 2;
        }
    }
}

static void reader_task(void *pv)
{
    FILE *f[2] = {NULL, NULL};
    char cur[2][DD_NAME_LEN] = {"", ""};
    uint32_t cur_ff[2] = {(uint32_t)-1, (uint32_t)-1};   // file frame each handle sits at
    int16_t *chunk = heap_caps_malloc(4096 * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    int16_t *tail  = heap_caps_malloc(DD_XFADE * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    s_alive = true;
    while (s_run) {
        reader_serve(&dd.d[0], &f[0], cur[0], &cur_ff[0], chunk, tail);
        reader_serve(&dd.d[1], &f[1], cur[1], &cur_ff[1], chunk, tail);
        vTaskDelay(1);   // >=1 tick (100 Hz): shorter is a busy-spin
    }
    for (int i = 0; i < 2; i++)
        if (f[i]) { sd_lock_take(); fclose(f[i]); sd_lock_give(); }
    free(chunk);
    free(tail);
    s_alive = false;
    vTaskDelete(NULL);
}

// ---- UI-side controls -------------------------------------------------------
int dualdeck_load_track(int deck, const char *name)
{
    dd_deck_t *v = &dd.d[deck & 1];
    v->playing = false;
    v->loading = true;
    v->track_bpm = 0;
    v->grid_offset = 0;
    v->phase_int = 0;
    v->phase_offset = 0;               // new track, new grid: drop the re-anchor
    v->sync_slew = 0;
    v->arm_start = v->arm_stop = false;
    strlcpy(v->track, name, sizeof(v->track));

    // tempo truth = the sidecar stamp (deck-analyzed or sampler3 take);
    // no analysis engine here — unstamped tracks free-run
    char jp[64];
    sample_resolve_aux(name, ".JSN", jp, sizeof(jp));
    cJSON *root = readJSONFileAsCJSON(jp);
    if (root) {
        cJSON *j;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "bpm")) && cJSON_IsNumber(j))
            v->track_bpm = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItemCaseSensitive(root, "grid")) && cJSON_IsNumber(j))
            v->grid_offset = (uint32_t)j->valuedouble;
        cJSON_Delete(root);
    }
    strlcpy(v->pending, name, sizeof(v->pending));
    v->track_req = true;   // reader opens + parks at the cue
    return 0;
}

void dualdeck_arm_start(int deck) { dd.d[deck & 1].arm_start = true; }
void dualdeck_arm_stop(int deck)  { dd.d[deck & 1].arm_stop = true; }

// LOOP toggle for one deck (audio task via TR2; ordered writes make it safe
// from the UI/web side too) — the deck's STREAMED loop, transplanted.
//
// A window must hold a WHOLE number of clock pulses or every wrap shifts the
// phase and the PLL spends the loop fighting it. One pulse = 4/ppb quarters.
static int dd_min_q(void)
{
    float ppb = dd_ppb[dd.ppb_idx] * (dd.ci.oct > 0 ? dd.ci.oct : 1.0f);
    int q = (ppb > 0) ? (int)(4.0f / ppb + 0.999f) : 4;
    return q < 1 ? 1 : q;
}

// LOOP KNOBS (deck parity, Arlo's call). While the FOCUSED deck is looping,
// CV6 becomes the loop WINDOW and CV7 the loop LENGTH — the deck's grammar,
// verbatim. On release both go back to filter/fader by PASS-THROUGH pickup: the
// knob is inert until it comes back THROUGH the value the engine is still using,
// so leaving a loop can never step the mix or slam the filter.
//   loop knobs:  -2 = grabbed (live), -1 = armed (dead until MOVED)
//   pickups:     -2 = live,           -1 = armed (dead until it CROSSES back)
#define DD_PICKUP  120        // counts of movement that grab a loop knob
#define DD_PASSTOL 90         // how close a knob must come to reclaim its param
static int s_cv6_ref = -2, s_cv7_ref = -2;   // loop window / length grabs
static int s_pk6 = -2, s_pk7 = -2;           // filter / fader pass-through
static int s_mv6 = 0, s_mv7 = 0;             // move debounce (ADC spikes)
static int s_c6_last = -1;                   // knob6 position the last move was made AT
static int s_len_idx = 4;                    // ladder index while looping

// a window move/resize takes effect AT THE READER'S FRONTIER: buffered audio
// plays out, the reader starts the new window, the cursor commits on arrival.
// Truncating the read-ahead to force it through faster STARVES the ring — and a
// starve freezes the cursor while the clock runs on, which is a phase slip.
static void dd_loop_remap(dd_deck_t *v, uint32_t new_start, uint32_t new_len)
{
    if (!v->loop_active || !new_len) return;
    if (new_start + new_len > v->file_frames) return;
    uint32_t at = v->wpos;                        // first frame the reader writes
    uint32_t off = (at - v->map_p0) % v->loop_len_fr;
    if (off >= new_len) off %= new_len;           // shrank under the cursor
    v->rm_start = new_start;
    v->rm_len = new_len;
    v->rm_p0 = at - off;                          // phase carries across the switch
    v->rm_at = at ? at : 1;                       // 0 means "none"
}
// the window in force for CONTROL purposes: the pending one if a move is
// scheduled. Comparing knobs against the COMMITTED window makes CV6 reschedule
// the same move forever, so it never commits (bench-caught on the deck).
static inline uint32_t dd_live_start(dd_deck_t *v){ return v->rm_at ? v->rm_start : v->loop_start; }
static inline uint32_t dd_live_len(dd_deck_t *v)  { return v->rm_at ? v->rm_len   : v->loop_len_fr; }

void dualdeck_loop_toggle(int deck)
{
    dd_deck_t *v = &dd.d[deck & 1];
    if (v->loop_active) {
        // RELEASE. Rebase the mapping to LINEAR at the cursor's current file
        // frame. Everything the reader already buffered from here to the next
        // wrap maps identically under both mappings, so it stays valid — the
        // ring keeps playing and there is no duck.
        uint32_t p = v->rpos_i;
        uint32_t off = (p - v->map_p0) % v->loop_len_fr;
        uint32_t ff = v->loop_start + off;
        uint32_t valid_to = p + (v->loop_len_fr - off);   // the next seam
        v->map_p0 = p;
        v->map_f0 = ff;
        v->rm_at = 0;
        v->loop_active = false;             // mapping written BEFORE the flag
        if (v->wpos > valid_to) v->wpos = valid_to;
        s_pk6 = s_pk7 = -1;                 // filter + fader stay put until the
                                            // knobs come back through them
        return;
    }
    // ENGAGE. Gates: mid-air states and gridless tracks can't loop.
    if (!v->playing || v->loading || v->seek_req ||
        v->track_bpm <= 20.0f || !v->file_frames) return;
    uint32_t beat_tf = (uint32_t)(60.0f * DD_RATE / v->track_bpm);
    if (!beat_tf) return;
    uint32_t ff = dd_map(v, v->rpos_i);
    if (ff >= v->file_frames) return;
    // anchor on the grid beat AT OR BEFORE the cursor (floor, not nearest): the
    // window must CONTAIN the cursor or the mapping can't stay continuous
    int64_t rel = (int64_t)ff - (int64_t)v->grid_offset;
    int64_t idx = (rel >= 0) ? rel / (int64_t)beat_tf
                             : -(((-rel) + beat_tf - 1) / (int64_t)beat_tf);
    int64_t st = (int64_t)v->grid_offset + idx * (int64_t)beat_tf;
    if (st < 0) st = 0;
    if (st > (int64_t)ff) st -= beat_tf;
    if (st < 0) st = 0;
    // clamp the Setup ladder to a whole clock pulse, then to the TRACK (the
    // ring is no longer the ceiling — the window streams)
    int minq = dd_min_q();
    int k = 0;
    while (k < DD_LOOP_STEPS - 1 && dd_loop_q[k] < dd.loop_len_beats) k++;
    while (k < DD_LOOP_STEPS - 1 && dd_loop_q[k] < minq) k++;
    uint32_t len = (uint32_t)((uint64_t)dd_loop_q[k] * beat_tf / 4);
    while (len > 0 && st + (int64_t)len > (int64_t)v->file_frames &&
           k > 0 && dd_loop_q[k - 1] >= minq) {
        k--;
        len = (uint32_t)((uint64_t)dd_loop_q[k] * beat_tf / 4);
    }
    if (!len || st + (int64_t)len > (int64_t)v->file_frames) return;
    // The window may be SHORTER than a beat, in which case the cursor's offset
    // into the beat can exceed the whole window. Slide the start forward by
    // whole windows so the cursor lands INSIDE it (still phase-true: the start
    // stays beat + n*window, and a window is a whole number of clock pulses).
    // Getting this wrong on the deck put wpos BEHIND rpos, the unsigned lead
    // underflowed, and the reader stopped filling forever — a permanent starve.
    if ((uint32_t)(ff - (uint32_t)st) >= len)
        st += (int64_t)(((uint32_t)(ff - (uint32_t)st)) / len) * (int64_t)len;
    if (st + (int64_t)len > (int64_t)v->file_frames) return;
    uint32_t off = ff - (uint32_t)st;             // cursor's offset into the window
    v->loop_start = (uint32_t)st;
    v->loop_len_fr = len;
    v->loop_beats = dd_loop_q[k];                 // QUARTER-beats (UI formats it)
    s_len_idx = k;
    s_cv6_ref = s_cv7_ref = -1;                   // loop knobs: dead until MOVED,
    s_mv6 = s_mv7 = 0;                            // so engaging can't fling the window
    s_c6_last = -1;
    v->map_p0 = v->rpos_i - off;                  // keeps map(rpos) == ff
    v->rm_at = 0;                                 // no stale scheduled window
    v->loop_active = true;                        // set LAST (write ordering)
    // the read-ahead stays valid up to the first seam; the reader wraps there
    uint32_t valid_to = v->map_p0 + len;
    if (v->wpos > valid_to) v->wpos = valid_to;
}

// RESYNC — the shared both-trig gesture (Arlo: "long press both tr1 tr2 forces
// resync of the pll with beat landing on release"). Applied to the FOCUSED deck,
// like every other trig here. The track slides to the nearest beat by a capped
// RATE BEND (never a seek — nothing drops), and the PLL's LOCK POINT moves with
// it, or the loop would quietly haul the beat back to the grid stamp and undo
// the gesture. This is also the manual nudge, and it costs no knob.
void dualdeck_resync(int deck)
{
    dd_deck_t *v = &dd.d[deck & 1];
    if (!v->playing || !v->track[0] || !v->file_frames || v->track_bpm <= 20.0f) return;
    uint32_t beat_tf = (uint32_t)(60.0f * DD_RATE / v->track_bpm);
    if (!beat_tf) return;
    int64_t rel = (int64_t)dd_map(v, v->rpos_i) - (int64_t)v->grid_offset;
    int64_t off = rel % (int64_t)beat_tf;
    if (off < 0) off += beat_tf;
    // nearer beat: the pull is never more than half a beat, so the bend is inaudible
    v->sync_slew = (off <= beat_tf / 2) ? -(float)off : (float)(beat_tf - off);
    if (dd.ci.clk.locked && dd.ci.clk.period > 0) {
        float p_ext = (float)dd.ci.clk.since / (float)dd.ci.clk.period;
        if (p_ext > 1.0f) p_ext = 1.0f;
        float po = p_ext + DD_LAG_LEAD_FR / (float)dd.ci.clk.period;
        po -= floorf(po);
        v->phase_offset = po;          // the release instant IS the beat
    }
    v->phase_int = 0;                  // start the new lock clean
}

// ---- engine helpers ----------------------------------------------------------
// fire the armed transport ops for one deck (called ON the bar boundary, or
// immediately when free-running with no clock)
static void deck_fire(int i)
{
    dd_deck_t *v = &dd.d[i];
    if (v->arm_stop) {
        v->arm_stop = false;
        v->arm_start = false;
        v->loop_active = false;        // stop exits the loop
        v->rm_at = 0;
        v->sync_slew = 0;              // no stale bend across a re-cue
        v->playing = false;
        // re-park at the cue so the next start is instant
        v->loading = true;
        v->seek_to = (v->grid_offset < v->file_frames) ? v->grid_offset : 0;
        v->seek_req = true;
        return;
    }
    if (v->arm_start) {
        v->arm_start = false;
        v->loop_active = false;        // a restart exits the loop
        v->rm_at = 0;
        v->sync_slew = 0;
        if (!v->track[0] || !v->file_frames) return;
        // "parked at the cue" is a question about the FILE position, not the
        // playback counter (the deck's hard lesson: counters carry a seek skew)
        if (v->playing || dd_map(v, v->rpos_i) != v->grid_offset || v->loading) {
            // restart / not parked: full seek protocol (brief refill mute)
            v->loading = true;
            v->seek_to = (v->grid_offset < v->file_frames) ? v->grid_offset : 0;
            v->seek_req = true;
        }
        v->phase_int = 0;
        v->playing = true;
        // TAKEOVER is opt-in now (Arlo: "it probably shouldn't auto crossfade
        // like that") — at fade_beats < 0 a deck start never touches the fader;
        // the mix is yours and starting a deck is purely a transport act.
        if (dd.fade_beats < 0) { /* off: the fader stays exactly where you left it */ }
        else if (dd.fade_beats > 0 && dd.ci.clk.locked && dd.ci.clk.period > 0) {
            float fade_frames = (float)dd.ci.clk.period * dd_ppb[dd.ppb_idx] * (float)dd.fade_beats;
            dd.auto_target = (i == 0) ? 0.0f : 1.0f;
            dd.auto_step = (fade_frames > 1) ? (float)(MACHINE_BLOCK / 2) / fade_frames : 1.0f;
            dd.auto_active = true;
            dd.manual = false;
            dd.xf_ref = dd.xf_cv;      // grab latch reference
            dd.grab_run = 0;
        } else if (dd.fade_beats == 0) {
            // cut: snap (still slewed a little in the block loop — no click)
            dd.auto_target = (i == 0) ? 0.0f : 1.0f;
            dd.auto_step = 1.0f;
            dd.auto_active = true;
            dd.manual = false;
            dd.xf_ref = dd.xf_cv;
            dd.grab_run = 0;
        }
    }
}

// one deck's PLL rate for this block (deck.c math, m = 1)
static float deck_rate(dd_deck_t *v)
{
    float rate = 1.0f;
    if (v->track_bpm > 20.0f && dd.ci.clk.locked && dd.ci.clk.period > 0) {
        uint32_t beat_tf = (uint32_t)(60.0f * DD_RATE / v->track_bpm);
        float seg_tf = (float)beat_tf / dd_ppb[dd.ppb_idx];
        float base = seg_tf / (float)dd.ci.clk.period;
        float p_ext = (float)dd.ci.clk.since / (float)dd.ci.clk.period;
        if (p_ext > 1.0f) p_ext = 1.0f;
        // FILE position, not the playback counter: the counter carries a
        // seek/loop skew, and locking against it puts the beat grid on a
        // SHIFTED reference — E still reads 0 while the deck is wrong (the
        // deck's silent-wrongness bug; do not "simplify" this back)
        float p_trk = fmodf((float)((int64_t)dd_map(v, v->rpos_i) - (int64_t)v->grid_offset),
                            seg_tf) / seg_tf;
        if (p_trk < 0) p_trk += 1.0f;
        float lead = DD_LAG_LEAD_FR / (float)dd.ci.clk.period;
        float err = p_ext - p_trk - v->phase_offset + lead;   // RESYNC moves the lock point
        err -= floorf(err);
        if (err > 0.5f) err -= 1.0f;
        // leaky PI (deck constants — instrument-verified there)
        v->phase_int = v->phase_int * 0.9999f + 0.0002f * err;
        if (v->phase_int > 0.04f) v->phase_int = 0.04f;
        else if (v->phase_int < -0.04f) v->phase_int = -0.04f;
        rate = base * (1.0f + 0.08f * err + v->phase_int);
        v->phase_err = err;
    } else {
        v->phase_int = 0;
        v->phase_err = 0;
    }
    if (rate < 0.25f) rate = 0.25f;
    if (rate > 2.5f) rate = 2.5f;
    v->rate_sm += 0.08f * (rate - v->rate_sm);
    v->rate = v->rate_sm;
    return v->rate_sm;
}

// ---- lifecycle ----------------------------------------------------------------
static esp_err_t dualdeck_start(void)
{
    memset(&dd, 0, sizeof(dd));
    for (int i = 0; i < 2; i++) {
        dd.d[i].ring = heap_caps_malloc((size_t)DD_RING_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!dd.d[i].ring) {
            ESP_LOGE(TAG, "PSRAM ring alloc failed (deck %d)", i);
            free(dd.d[0].ring); dd.d[0].ring = NULL;
            return ESP_ERR_NO_MEM;
        }
        dd.d[i].rate = 1.0f;
        dd.d[i].rate_sm = 1.0f;
    }
    dd.clk_src = 7;                 // CV8, house convention
    dd.ppb_idx = 4;                 // 4 PPQN, the modular norm
    dd.fade_beats = -1;             // takeover OFF by default
    dd.loop_len_beats = 16;         // QUARTER-beats = 4 beats (ladder v2)
    dd.layout = DD_LAY_V;           // stacked single-decks
    dd.xf = 0.0f;
    dd.manual = true;
    clockin_reset(&dd.ci, dd_ppb[dd.ppb_idx]);
    s_run = true;
    xTaskCreate(reader_task, "dd_reader", 4096, NULL, 6, NULL);
    audio_status_set_voices("doubledecker", "");
    return ESP_OK;
}

static void dualdeck_stop(void)
{
    dd.d[0].playing = dd.d[1].playing = false;
    s_run = false;
    for (int i = 0; i < 100 && s_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free(dd.d[0].ring); dd.d[0].ring = NULL;
    free(dd.d[1].ring); dd.d[1].ring = NULL;
}

// ---- process --------------------------------------------------------------------
static void dualdeck_process(int32_t out[MACHINE_BLOCK],
                             const int32_t in[MACHINE_BLOCK],
                             const machine_io_t *io)
{
    if (!dd.d[0].ring || !dd.d[1].ring) return;
    const int frames = MACHINE_BLOCK / 2;

    // ---- trig gates: the UNIFIED grammar on the FOCUSED deck (Arlo:
    // consistency with deck/tracker beats per-deck trigs). TR1 tap =
    // quantized start/restart, TG_HOLD = quantized stop; TR2 press = LOOP
    // toggle (beat-true), held past 0.6 s = momentary (long release exits).
    static trig_gate_t tg[2];
    static trig_combo_t tc;
    int fo = dd.focus;
    bool d1 = !(io->trig_level & 1), d2 = !(io->trig_level & 2);
    tg_event_t e1 = trig_gate_step(&tg[0], d1, frames);
    tg_event_t e2 = trig_gate_step(&tg[1], d2, frames);
    // BOTH-TRIG RESYNC on the focused deck. It arms at 0.35 s — ahead of either
    // gate's own 0.6 s hold — and while it owns the trigs their individual
    // events are swallowed, or the same gesture would also stop the deck and
    // flip its loop on the way past.
    tc_event_t ec = trig_combo_step(&tc, d1, d2, frames);
    if (ec == TC_FIRE) dualdeck_resync(fo);
    if (ec == TC_ARMED) dd.d[fo].resync_armed = true;
    if (!trig_combo_busy(&tc)) {
        dd.d[0].resync_armed = dd.d[1].resync_armed = false;
        if (e1 == TG_HOLD) dualdeck_arm_stop(fo);
        else if (e1 == TG_REL_SHORT) dualdeck_arm_start(fo);
        if (e2 == TG_PRESS || e2 == TG_REL_LONG) dualdeck_loop_toggle(fo);
    }

    // ---- shared clock + bar phase. An accepted pulse advances the counter;
    // the bar boundary is every 4 beats' worth of pulses. With no lock, armed
    // ops fire immediately (free-run behaviour).
    clockin_set_ppb(&dd.ci, dd_ppb[dd.ppb_idx]);
    uint32_t rn_pre = dd.ci.clk.ring_n;
    clockin_block(&dd.ci, io->cv[dd.clk_src & 7], frames);
    bool pulse = (dd.ci.clk.ring_n != rn_pre);
    uint32_t per_bar = (uint32_t)(dd_ppb[dd.ppb_idx] * 4.0f + 0.5f);
    if (per_bar < 1) per_bar = 1;
    if (!dd.ci.clk.locked) dd.pulses = 0;
    else if (pulse) dd.pulses++;
    bool bar_edge = pulse && dd.ci.clk.locked && (dd.pulses % per_bar) == 1;
    if (bar_edge || !dd.ci.clk.locked) { deck_fire(0); deck_fire(1); }

    // ---- LOOP KNOBS: while the FOCUSED deck loops, CV6/CV7 belong to the loop
    // (window / length) — the same deck the trigs address. Everything is done on
    // the LIVE (pending-aware) window, or a move reschedules itself forever.
    int c6 = io->cv[5], c7 = io->cv[6];
    dd_deck_t *fv = &dd.d[dd.focus];
    bool borrow = fv->loop_active && fv->track_bpm > 20.0f && fv->file_frames;
    if (borrow) {
        uint32_t beat_tf = (uint32_t)(60.0f * DD_RATE / fv->track_bpm);
        if (beat_tf) {
            // ARM: seize the reference the block after engage. Without this the
            // refs sat at -1 forever and the loop knobs never went live at all
            // (the deck does this; I ported the grab test and not the arm).
            if (s_cv6_ref == -1) { s_cv6_ref = c6; s_mv6 = 0; }
            if (s_cv7_ref == -1) { s_cv7_ref = c7; s_mv7 = 0; }
            // CV7 = LENGTH. Grab-then-track: dead until the knob MOVES, so
            // engaging a loop can't instantly resize it.
            if (s_cv7_ref >= 0 &&
                (c7 - s_cv7_ref > DD_PICKUP || s_cv7_ref - c7 > DD_PICKUP)) s_cv7_ref = -2;
            if (s_cv7_ref == -2) {
                int minq = dd_min_q();
                int ni = (int)((uint64_t)c7 * DD_LOOP_STEPS / 4096);
                if (ni >= DD_LOOP_STEPS) ni = DD_LOOP_STEPS - 1;
                while (ni < DD_LOOP_STEPS - 1 && dd_loop_q[ni] < minq) ni++;
                if (ni != s_len_idx && ++s_mv7 >= 3) {
                    s_mv7 = 0;
                    uint32_t nl = (uint32_t)((uint64_t)dd_loop_q[ni] * beat_tf / 4);
                    uint32_t cs = dd_live_start(fv);
                    if (nl && cs + nl <= fv->file_frames) {
                        s_len_idx = ni;
                        fv->loop_beats = dd_loop_q[ni];
                        dd_loop_remap(fv, cs, nl);
                    }
                } else if (ni == s_len_idx) s_mv7 = 0;
            }
            // CV6 = WINDOW, absolute across the whole track (one sweep spans it).
            // It acts only when the KNOB moves: re-evaluating it on a CV7 length
            // change would re-quantize the same knob position onto the new,
            // coarser window grid and drag the start backwards.
            if (s_cv6_ref >= 0 &&
                (c6 - s_cv6_ref > DD_PICKUP || s_cv6_ref - c6 > DD_PICKUP)) s_cv6_ref = -2;
            uint32_t llen = dd_live_len(fv);
            int moved = (s_c6_last < 0) || (c6 - s_c6_last > 24) || (s_c6_last - c6 > 24);
            if (s_cv6_ref == -2 && llen && moved) {
                uint32_t span = (fv->file_frames > fv->grid_offset)
                              ? fv->file_frames - fv->grid_offset : 0;
                uint32_t nwin = span / llen;
                if (nwin) {
                    uint32_t idx = (uint32_t)((uint64_t)c6 * nwin / 4096);
                    if (idx >= nwin) idx = nwin - 1;
                    uint32_t ns = fv->grid_offset + idx * llen;
                    if (ns != dd_live_start(fv) && ++s_mv6 >= 3) {
                        s_mv6 = 0;
                        s_c6_last = c6;
                        if (ns + llen <= fv->file_frames) dd_loop_remap(fv, ns, llen);
                    } else if (ns == dd_live_start(fv)) { s_mv6 = 0; s_c6_last = c6; }
                }
            }
        }
    } else {
        // PASS-THROUGH pickup: a knob the loop borrowed stays inert until it
        // returns to the value the engine is still using — then it takes over
        // and nothing jumps (the deck's 2x/half-speed slam, avoided here).
        if (s_pk6 != -2) {
            int d = c6 - dd.filt_cv;
            if (d < 0) d = -d;
            if (d <= DD_PASSTOL) s_pk6 = -2;
        }
        if (s_pk7 != -2) {
            int d = c7 - (int)(dd.xf * 4095.0f);   // the fader's LIVE position
            if (d < 0) d = -d;
            if (d <= DD_PASSTOL) s_pk7 = -2;
        }
    }
    bool flt_live = !borrow && s_pk6 == -2;   // else the filter holds its value
    bool xf_live  = !borrow && s_pk7 == -2;   // else the mix stays exactly put

    // ---- crossfade: three states — AUTO (takeover fade in flight), HELD
    // (fade landed; the mix stays put wherever automation left it), MANUAL
    // (knob is live). Moving the knob past the grab deadband promotes
    // auto/held to manual — the pickup. Handing straight back to manual on
    // fade completion would snap the mix to wherever the knob happens to
    // sit, defeating the takeover entirely (caught on first bench test).
    dd.xf_cv = c7;                 // knob7 = crossfade (CV6 is the filter, house rule)
    if (!dd.manual && xf_live) {       // auto or held: watch for the grab.
        // The move must PERSIST (~12 ms) — a single-block WiFi ADC spike on
        // the knob read faked a grab and killed every takeover fade the
        // moment /status polling was active (sampler3's median-of-5 lesson).
        int dcv = dd.xf_cv - dd.xf_ref;
        if (dcv < 0) dcv = -dcv;
        if (dcv > DD_XF_GRAB) {
            if (++dd.grab_run >= 8) { dd.auto_active = false; dd.manual = true; }
        } else dd.grab_run = 0;
    }
    if (dd.auto_active) {
        float xf_target = dd.auto_target;
        float step = dd.auto_step;
        if (dd.xf < xf_target) { dd.xf += step; if (dd.xf > xf_target) dd.xf = xf_target; }
        else                   { dd.xf -= step; if (dd.xf < xf_target) dd.xf = xf_target; }
        if (dd.xf == xf_target) dd.auto_active = false;   // -> HELD, not manual
    } else if (dd.manual && xf_live) {
        float xf_target = (float)dd.xf_cv / 4095.0f;
        // gentle slew so a jumpy ADC read never steps the mix
        dd.xf += 0.2f * (xf_target - dd.xf);
    }
    if (dd.xf < 0) dd.xf = 0;
    if (dd.xf > 1) dd.xf = 1;
    // equal-power gains
    float ga = cosf(dd.xf * (float)M_PI_2);
    float gb = sinf(dd.xf * (float)M_PI_2);

    // ---- master DJ filter on knob6 (Arlo: "reverse the cv6/7 assignments, that
    // way filter freq stays on cv6 like other machines" — the deck, drums and
    // looper all put the sweep there; crossfade moves to knob7)
    if (flt_live) dd.filt_cv = c6;     // else FROZEN (loop has the knob, or the
    int fcv = dd.filt_cv;              // knob has not come back through it yet)
    int mode = 0;
    float fc = 0;
    if (fcv < 2048 - 150) {
        mode = 1;
        float t = (float)fcv / (2048.0f - 150.0f);
        fc = 80.0f * powf(150.0f, t);
    } else if (fcv > 2048 + 150) {
        mode = 2;
        float t = (float)(fcv - 2048 - 150) / (4095.0f - 2048.0f - 150.0f);
        fc = 30.0f * powf(200.0f, t);
    }
    dd.flt_mode = mode;
    float f_target = mode ? svf_coef(fc, (float)DD_RATE, 1.2f) : 0;
    dd.flt_f += 0.2f * (f_target - dd.flt_f);
    const float q = 0.9f;

    // ---- per-deck rate for this block
    float rate[2];
    for (int i = 0; i < 2; i++) rate[i] = deck_rate(&dd.d[i]);

    // ---- render
    static float last[2][2];       // per-deck declick tails
    bool starved[2] = {false, false};
    for (int fno = 0; fno < frames; fno++) {
        float mix_l = 0, mix_r = 0;
        for (int i = 0; i < 2; i++) {
            dd_deck_t *v = &dd.d[i];
            float g = (i == 0) ? ga : gb;
            // wpos is the reader's frontier in PLAYBACK space; the file end only
            // bounds playback when NOTHING is windowing it (a loop never reaches it)
            bool can_play = v->playing && !v->loading && v->rpos_i + 1 < v->wpos;
            uint32_t ff_now = dd_map(v, v->rpos_i);
            if (!can_play && v->playing && !v->loading && !v->seek_req &&
                v->file_frames && (v->loop_active || v->tl_len || ff_now + 1 < v->file_frames))
                starved[i] = true;
            float gt = can_play ? 1.0f : 0.0f;
            v->out_gain += (gt - v->out_gain) * 0.015f;
            float l, r;
            if (!can_play) {
                // EOF only exists when nothing windows playback. The track loop
                // now wraps INSIDE the mapping — phase-exact, no seek, no PLL
                // reset (the old wrap-to-cue re-buffered and re-locked every pass)
                if (v->playing && !v->loading && !v->loop_active && !v->tl_len &&
                    v->file_frames && ff_now + 1 >= v->file_frames && !v->seek_req) {
                    v->loading = true;
                    v->seek_to = (v->grid_offset < v->file_frames) ? v->grid_offset : 0;
                    v->seek_req = true;
                    v->phase_int = 0;
                }
                last[i][0] *= 0.94f;
                last[i][1] *= 0.94f;
                l = last[i][0]; r = last[i][1];
            } else {
                uint32_t i0 = v->rpos_i % DD_RING_FRAMES;
                uint32_t i1 = (i0 + 1) % DD_RING_FRAMES;
                float fr = (float)v->rpos_f;
                l = (float)v->ring[i0 * 2]     + ((float)v->ring[i1 * 2]     - (float)v->ring[i0 * 2])     * fr;
                r = (float)v->ring[i0 * 2 + 1] + ((float)v->ring[i1 * 2 + 1] - (float)v->ring[i0 * 2 + 1]) * fr;
                last[i][0] = l * v->out_gain;
                last[i][1] = r * v->out_gain;
                l = last[i][0]; r = last[i][1];
                // RESYNC catch-up: bleed the shift in as a capped rate bend
                // (rate >= 0.25 and |sl| <= 0.15, so playback never runs backwards)
                float sl = v->sync_slew * DD_SLEW_GAIN;
                if (sl > DD_SLEW_MAX) sl = DD_SLEW_MAX;
                else if (sl < -DD_SLEW_MAX) sl = -DD_SLEW_MAX;
                v->sync_slew -= sl;
                v->rpos_f += rate[i] + sl;
                // playback counter: NEVER wraps. The loop lives in the MAPPING
                // (the reader wraps its own file reads and crossfades the seam),
                // so there is no engine-side cursor wrap any more.
                while (v->rpos_f >= 1.0) { v->rpos_f -= 1.0; v->rpos_i++; }
            }
            mix_l += l * g;
            mix_r += r * g;
        }
        if (mode) {
            float lo, hi;
            svf_step(&dd.flt_l, mix_l, dd.flt_f, q, &lo, NULL, &hi);
            mix_l = (mode == 1) ? lo : hi;
            svf_step(&dd.flt_r, mix_r, dd.flt_f, q, &lo, NULL, &hi);
            mix_r = (mode == 1) ? lo : hi;
        } else {
            svf_park(&dd.flt_l, mix_l);
            svf_park(&dd.flt_r, mix_r);
        }
        if (mix_l > 32767) mix_l = 32767;
        if (mix_l < -32768) mix_l = -32768;
        if (mix_r > 32767) mix_r = 32767;
        if (mix_r < -32768) mix_r = -32768;
        out[fno * 2]     = ((int32_t)mix_l) << 16;
        out[fno * 2 + 1] = ((int32_t)mix_r) << 16;
    }
    if (starved[0]) dd.d[0].dbg_starve++;
    if (starved[1]) dd.d[1].dbg_starve++;

    for (int i = 0; i < 2; i++) {
        dd_deck_t *v = &dd.d[i];
        // a scheduled window becomes the live one once the cursor reaches it
        if (v->loop_active && v->rm_at && v->rpos_i >= v->rm_at) {
            v->loop_start = v->rm_start;
            v->loop_len_fr = v->rm_len;
            v->map_p0 = v->rm_p0;
            v->rm_at = 0;
        }
        v->ui_fpos = dd_map(v, v->rpos_i);      // the UI reads FILE position
        v->ui_lstart = v->rm_at ? v->rm_start : v->loop_start;
        v->ui_llen = v->rm_at ? v->rm_len : v->loop_len_fr;
        // keep the track-loop window in step with the sidecar stamp (no I/O)
        uint32_t sig = (v->grid_offset << 1) ^ ((uint32_t)(v->track_bpm * 100) << 3) ^
                       v->file_frames;
        static uint32_t s_tl_sig[2] = {0, 0};
        if (sig != s_tl_sig[i]) { s_tl_sig[i] = sig; dd_tl_update(v); }
    }
}

// ---- preset -------------------------------------------------------------------
static cJSON *dualdeck_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ta", dd.d[0].track);
    cJSON_AddStringToObject(o, "tb", dd.d[1].track);
    cJSON_AddNumberToObject(o, "clk_src", dd.clk_src);
    cJSON_AddNumberToObject(o, "ppb", dd.ppb_idx);
    cJSON_AddNumberToObject(o, "fade", dd.fade_beats);
    cJSON_AddNumberToObject(o, "llen", dd.loop_len_beats);
    cJSON_AddNumberToObject(o, "lq", 1);      // llen units = QUARTER-beats
    cJSON_AddNumberToObject(o, "lay", dd.layout);
    return o;
}

static void dualdeck_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j)) {
        dd.clk_src = j->valueint & 7;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ppb")) && cJSON_IsNumber(j)) {
        dd.ppb_idx = j->valueint;
        if (dd.ppb_idx < 0) dd.ppb_idx = 0;
        if (dd.ppb_idx > 5) dd.ppb_idx = 5;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fade")) && cJSON_IsNumber(j)) {
        int fb = j->valueint;
        dd.fade_beats = (fb == -1 || fb == 0 || fb == 1 || fb == 4 || fb == 8) ? fb : -1;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "llen")) && cJSON_IsNumber(j)) {
        // "llen" is in QUARTER-beats since the streamed-loop port ("lq":1). An
        // OLD autosave holds whole beats, and the two units overlap on the
        // ladder (4 = "4 beats" then, "1 beat" now), so a silent reinterpretation
        // would quarter every stored loop. The version key disambiguates.
        int lb = j->valueint;
        cJSON *ver = cJSON_GetObjectItemCaseSensitive(node, "lq");
        if (!(ver && cJSON_IsNumber(ver) && ver->valueint >= 1)) lb *= 4;   // migrate
        int ok = 0;
        for (int k = 0; k < DD_LOOP_STEPS; k++) if (dd_loop_q[k] == lb) ok = 1;
        dd.loop_len_beats = ok ? lb : 16;  // 4 beats
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lay")) && cJSON_IsNumber(j))
        dd.layout = (j->valueint == DD_LAY_H) ? DD_LAY_H : DD_LAY_V;
    // track loads only when the value actually changes — preset_load also runs
    // on remote settings writes, and reloading mid-performance would mute
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ta")) && cJSON_IsString(j) &&
        j->valuestring[0] && strcmp(j->valuestring, dd.d[0].track) != 0)
        dualdeck_load_track(0, j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "tb")) && cJSON_IsString(j) &&
        j->valuestring[0] && strcmp(j->valuestring, dd.d[1].track) != 0)
        dualdeck_load_track(1, j->valuestring);
}

extern const machine_ui_t dualdeck_menu_ui;

const machine_t machine_dualdeck = {
    .name = "DoubleDecker",
    .start = dualdeck_start,
    .stop = dualdeck_stop,
    .process = dualdeck_process,
    .preset_save = dualdeck_preset_save,
    .preset_load = dualdeck_preset_load,
    .ui = &dualdeck_menu_ui,
};
