// Multi-mode reverb — Dattorro-style plate tank (Effect Design Part 1
// topology, delays rescaled 29.761 kHz -> 44.1 kHz), modes as parameter sets,
// shimmer via an octave-up dual-head shifter in the tank feed.
//
// Numbers stay float end to end; every delay line is carved out of one PSRAM
// slab. Per sample: ~26 line accesses + ~60 flops — measured live via
// rv->cost_us (EMA, us per 64-frame block; 1450 us = 100% of the audio tick).
// No tank modulation in v1 — add the classic ±8-sample excursion later if the
// tail reads metallic on hardware.
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "reverb.h"
#include "fxchain.h"

// Declared here rather than by including audio.h: components/audio already
// REQUIRES util, so util cannot require audio back without a cycle. Safe to
// resolve at link time — audio is CORE and present in every image, including the
// machine-less proof build. See audio.h for what these two numbers mean.
void audio_rv_report(int wet_pk_pct, bool nan_flush);
void audio_rv_cost(int us);
bool audio_fx_meters_armed(void);

static const char *TAG = "REVERB";

// base line capacities (samples @44.1k; Dattorro x1.4818, rounded)
#define L_PRE    1102     // 25 ms predelay
#define L_API0   210
#define L_API1   159
#define L_API2   562
#define L_API3   410
#define L_APA1   996
#define L_DA1    6598
#define L_APA2   2667
#define L_DA2    5512
#define L_APB1   1345
#define L_DB1    6249
#define L_APB2   3936
#define L_DB2    4687
#define L_SHIM   4096     // ~93 ms shifter window
#define RV_SLAB  (L_PRE+L_API0+L_API1+L_API2+L_API3+L_APA1+L_DA1+L_APA2+L_DA2+ \
                  L_APB1+L_DB1+L_APB2+L_DB2+L_SHIM)

// output tap offsets (into the live, size-scaled lines; scaled on mode set)
static const int TAP_BASE[14] = {
    // left: +d_b1[394], +d_b1[4407], -ap_b2[2835], +d_b2[2958],
    //       -d_a1[2949], -ap_a2[277], -d_a2[1580]
    394, 4407, 2835, 2958, 2949, 277, 1580,
    // right: +d_a1[523], +d_a1[5375], -ap_a2[1820], +d_a2[3961],
    //        -d_b1[3128], -ap_b2[496], -d_b2[179]
    523, 5375, 1820, 3961, 3128, 496, 179,
};

static inline float line_read(const rv_line_t *l, int back)
{
    int i = l->w - back;
    if (i < 0) i += l->len;
    return l->buf[i];
}
static inline void line_push(rv_line_t *l, float v)
{
    l->buf[l->w] = v;
    if (++l->w >= l->len) l->w = 0;
}
// classic allpass: y = -g*x + delayed;  push x + g*y
static inline float ap_step(rv_line_t *l, float x, float g)
{
    float d = line_read(l, l->len - 1);
    float y = d - g * x;
    line_push(l, x + g * y);
    return y;
}

static float *carve(float **p, rv_line_t *l, int cap)
{
    l->buf = *p;
    l->cap = cap;
    l->len = cap;
    l->w = 0;
    *p += cap;
    return l->buf;
}

esp_err_t reverb_init(reverb_t *rv)
{
    memset(rv, 0, sizeof(*rv));
    rv->slab = heap_caps_calloc(RV_SLAB, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!rv->slab) {
        ESP_LOGE(TAG, "PSRAM slab alloc failed (%u KB)",
                 (unsigned)(RV_SLAB * sizeof(float) / 1024));
        return ESP_ERR_NO_MEM;
    }
    float *p = rv->slab;
    carve(&p, &rv->pre, L_PRE);
    carve(&p, &rv->ap_in[0], L_API0);
    carve(&p, &rv->ap_in[1], L_API1);
    carve(&p, &rv->ap_in[2], L_API2);
    carve(&p, &rv->ap_in[3], L_API3);
    carve(&p, &rv->ap_a1, L_APA1);
    carve(&p, &rv->d_a1,  L_DA1);
    carve(&p, &rv->ap_a2, L_APA2);
    carve(&p, &rv->d_a2,  L_DA2);
    carve(&p, &rv->ap_b1, L_APB1);
    carve(&p, &rv->d_b1,  L_DB1);
    carve(&p, &rv->ap_b2, L_APB2);
    carve(&p, &rv->d_b2,  L_DB2);
    carve(&p, &rv->shim,  L_SHIM);
    rv->mode = RV_OFF;
    rv->wet = 0.35f;
    rv->fade_g = 1.0f; rv->fade_tgt = 1.0f;   // open; set_mode drives the ramp
    rv->clear_off = -1;                       // not flushing
    ESP_LOGI(TAG, "slab %u KB PSRAM", (unsigned)(RV_SLAB * sizeof(float) / 1024));
    return ESP_OK;
}

void reverb_free(reverb_t *rv)
{
    if (rv->slab) heap_caps_free(rv->slab);
    rv->slab = NULL;
    rv->mode = RV_OFF;
}

// live tap offsets, rebuilt when size scales the tank
static int s_tap[14];

static void tank_resize(reverb_t *rv, float size)
{
    if (size < 0.35f) size = 0.35f;
    if (size > 1.0f) size = 1.0f;
    rv_line_t *tank[8] = { &rv->ap_a1, &rv->d_a1, &rv->ap_a2, &rv->d_a2,
                           &rv->ap_b1, &rv->d_b1, &rv->ap_b2, &rv->d_b2 };
    for (int i = 0; i < 8; i++) {
        int nl = (int)((float)tank[i]->cap * size);
        if (nl < 4) nl = 4;
        tank[i]->len = nl;
        tank[i]->w = 0;
        // NO memset here: these 8 lines are 124 KB and this ran as one unchunked
        // burst, which is most of what overran the audio block (MEASURED: 2282 us
        // against a 1450 us budget at the moment of the switch). rv_clear_slab
        // covers this same memory in bounded chunks and is now the ONLY place
        // the tank is zeroed.
    }
    for (int i = 0; i < 14; i++) {
        s_tap[i] = (int)((float)TAP_BASE[i] * size);
        if (s_tap[i] < 1) s_tap[i] = 1;
    }
    rv->damp_a = rv->damp_b = 0;
    rv->in_lp = 0;
    rv->shim_lp = 0;
    rv->shim_dc = 0;
}

// Shimmer feedback stability. The octave-up shifter doubles frequency every
// pass, so without loss the tail BLOOMS upward until it pins at full-scale on
// silence (measured: RMS climbs to clip and stays there). Two brakes:
//   SHIM_FB_LP  — one-pole LP on the return: once energy climbs past this
//                 cutoff it's removed, killing the upward runaway.
//   the mode's shim_gain is the loop gain for the low/mid band the LP can't
//   catch; it must keep the whole loop < 1 so a silent tail DECAYS.
#define SHIM_FB_LP  0.35f
// ceiling on the shimmer feedback injection (half full scale) — see tank_step
#define SHIM_CEIL   16000.0f

// The heavy part: swap the parameter set, resize and clear the tank. Callable
// from AUDIO context (the NaN guard does) because it never sleeps — the fade
// lives in reverb_set_mode() around it.
// Clearing the tank is ~170 KB of PSRAM writes. As ONE memset that saturates the
// PSRAM bus for 10-15 ms, which starves the audio task's own PSRAM reads (on
// Tape, the tape banks themselves) and clicks no matter what the reverb signal
// is doing — a fade cannot mask it because it is not a signal discontinuity.
// Chunked + yielding when we're in UI context. The NaN guard used to take the
// straight path here because it runs in the audio task with nothing to yield to
// — which made every firing a guaranteed dropout, since 10-15 ms of held bus is
// longer than the WHOLE I2S DMA buffer (8.7 ms). It now hands the work to the
// incremental flush (rv_flush_tick) instead and never calls this.
static void rv_clear_slab(reverb_t *rv, bool may_yield)
{
    rv->clear_off = -1;          // a full clear supersedes any in-flight flush
    const size_t total = (size_t)RV_SLAB * sizeof(float);
    if (!may_yield) { memset(rv->slab, 0, total); return; }
    // 8 KB per chunk ~= 0.5 ms of PSRAM writes, comfortably inside one 1.45 ms
    // audio block. 32 KB was 2.1 ms — a single chunk blew the deadline by itself.
    const size_t CH = 8192;
    for (size_t off = 0; off < total; off += CH) {
        size_t n = (total - off < CH) ? (total - off) : CH;
        memset((uint8_t *)rv->slab + off, 0, n);
        vTaskDelay(1);                      // let the audio task have the bus back
    }
}

static void reverb_apply_mode(reverb_t *rv, int mode, bool may_yield)
{
    if (!rv->slab) { rv->mode = RV_OFF; return; }
    // mute the tank FIRST (the kernel gates on rv->mode) so the audio task
    // outputs dry while we reconfigure — otherwise a torn param read + the
    // residual tail meeting the new decay/shim gain cracks like an explosion
    // on switch-in.
    rv->mode = RV_OFF;
    float size = 1.0f;
    switch (mode) {
        case RV_ROOM:
            size = 0.45f; rv->decay = 0.42f; rv->damp = 0.55f;
            rv->in_bw = 0.55f; rv->shim_gain = 0.0f;
            break;
        case RV_HALL:
            size = 1.0f;  rv->decay = 0.62f; rv->damp = 0.35f;
            rv->in_bw = 0.65f; rv->shim_gain = 0.0f;
            break;
        case RV_PLATE:
            size = 0.75f; rv->decay = 0.55f; rv->damp = 0.10f;
            rv->in_bw = 0.90f; rv->shim_gain = 0.0f;
            break;
        case RV_SHIMMER:
            size = 1.0f;  rv->decay = 0.63f; rv->damp = 0.25f;
            rv->in_bw = 0.70f; rv->shim_gain = 0.38f;
            break;
        default:
            mode = RV_OFF;
            break;
    }
    if (mode != RV_OFF) {
        tank_resize(rv, size);
        // clear EVERY line (pre + diffusers + tank + shim window) so the switch
        // starts from true silence — no residual energy to detonate under the
        // new mode's gain. Chunked (see rv_clear_slab); lengths/states were just
        // reset by tank_resize.
        rv_clear_slab(rv, may_yield);
    }
    rv->shim_pos = 0;
    rv->mode = mode;                 // set LAST: the kernel gates on it
}

// FADED mode change (Arlo 2026-07-25: "a beep or a click when changing
// reverbs"). Clearing the tank cut a ringing tail to zero in ONE sample, and a
// step that size is a click; the new mode then came in at full return level.
// So: ask the audio task to ramp the return down, WAIT for it (UI context — a
// few ticks here is imperceptible), reconfigure into the silence, then ramp
// back up. The wait is bounded: if the kernel isn't running (stopped machine,
// mode already OFF) fade_g never moves and we just proceed.
void reverb_set_mode(reverb_t *rv, int mode)
{
    if (!rv->slab) { rv->mode = RV_OFF; return; }
    if (rv->mode != RV_OFF) {
        rv->fade_tgt = 0.0f;
        for (int i = 0; i < 6 && rv->fade_g > 0.01f; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    reverb_apply_mode(rv, mode, true);
    rv->fade_g  = 0.0f;              // new tank starts silent...
    rv->fade_tgt = 1.0f;             // ...and fades in over RV_FADE_MS
}

void reverb_set_mix(reverb_t *rv, float wet)
{
    if (wet < 0) wet = 0;
    if (wet > 1) wet = 1;
    rv->wet = wet;
}

// octave-up dual-head granular over the shifter window: heads advance at 2x,
// half a window apart, triangle-crossfaded — integer reads, no interpolation
static inline float shim_step(reverb_t *rv, float x)
{
    line_push(&rv->shim, x);
    float pos = rv->shim_pos + 2.0f;
    if (pos >= (float)L_SHIM) pos -= (float)L_SHIM;
    rv->shim_pos = pos;
    int n = L_SHIM, h = n / 2;
    int p0 = (int)pos;
    int back0 = rv->shim.w - 1 - p0; if (back0 < 0) back0 += n;
    int back1 = back0 - h; if (back1 < 0) back1 += n;
    // Crossfade window on head 0's phase; head 1 gets the complement.
    // SMOOTHSTEPPED (2026-07-26): the bare triangle is continuous but its
    // DERIVATIVE jumps at the apex and at the wrap, and a kink in an amplitude
    // envelope splatters spectrally — at this grain rate (~21.5 Hz) that is
    // heard as periodic clicks. Alone, the tank's diffusion hides it; behind a
    // FLANGER the two heads read material 46 ms apart at different comb phase,
    // the mismatch at each boundary is far bigger, and it turns into the "clicky
    // scratchyness" Arlo heard on flanger+shimmer. smoothstep has zero slope at
    // both ends, so the boundaries stop being corners. Still complementary
    // (w0 + w1 == 1) and trig-free — this runs per sample in the audio task.
    float ph = pos / (float)n;               // 0..1
    float tri = 2.0f * (ph < 0.5f ? ph : 1.0f - ph);
    float w0 = tri * tri * (3.0f - 2.0f * tri);
    return line_read(&rv->shim, back0) * w0 +
           line_read(&rv->shim, back1) * (1.0f - w0);
}

// return-level fade, stepped once per frame. ~20 ms end to end: long enough to
// kill the click, short enough that a mode change still feels instant.
#define RV_FADE_STEP (1.0f / 882.0f)
static inline float rv_fade_step(reverb_t *rv)
{
    if (rv->fade_g != rv->fade_tgt) {
        rv->fade_g += (rv->fade_tgt > rv->fade_g) ? RV_FADE_STEP : -RV_FADE_STEP;
        if (rv->fade_g > 1.0f) rv->fade_g = 1.0f;
        if (rv->fade_g < 0.0f) rv->fade_g = 0.0f;
        if (fabsf(rv->fade_g - rv->fade_tgt) < RV_FADE_STEP) rv->fade_g = rv->fade_tgt;
    }
    return rv->fade_g;
}

// one tank sample: feed x in, get the stereo taps out. Shared by the insert
// and send paths (the tank itself never knew the difference).
static inline void tank_step(reverb_t *rv, float x, float decay, float damp,
                             float bw, float sg, float *yl, float *yr)
{
    // shimmer: blend the octave-up of what the tank just produced, low-passed
    // and gain-limited so the feedback loop stays < 1 (else it blooms to
    // full-scale on silence — see SHIM_FB_LP), and DC-BLOCKED so a tiny offset
    // can't circulate the loop and slowly rail the tank to silence over minutes
    // (the "shimmer fades all the way out / needs a reset" drift).
    if (sg > 0) {
        float sh = shim_step(rv, rv->damp_a + rv->damp_b);
        rv->shim_lp += SHIM_FB_LP * (sh - rv->shim_lp);        // LP: tame the upward climb
        rv->shim_dc += 0.0007f * (rv->shim_lp - rv->shim_dc);  // track the slow DC
        // BOUND THE INJECTION. sg is tuned for a broadly flat source; a RESONANT
        // one (a flanger's comb peaks) raises the effective loop gain at those
        // frequencies and the octave-up loop blooms — every other reverb mode was
        // fine behind a flanger and shimmer alone went bad (Arlo 2026-07-26).
        // Exactly linear below the knee, like fx_pack_softclip: normal shimmer is
        // untouched, a runaway simply cannot get out of hand.
        float si = rv->shim_lp - rv->shim_dc;
        float sa = fabsf(si);
        if (sa > SHIM_CEIL) {
            float over = sa - SHIM_CEIL;
            si = (si < 0 ? -1.0f : 1.0f) * (SHIM_CEIL + over / (1.0f + over / SHIM_CEIL));
        }
        x += sg * si;                                          // inject DC-free
    }

    line_push(&rv->pre, x);
    x = line_read(&rv->pre, rv->pre.len - 1);
    rv->in_lp += bw * (x - rv->in_lp);           // input bandwidth
    x = rv->in_lp;
    x = ap_step(&rv->ap_in[0], x, 0.75f);
    x = ap_step(&rv->ap_in[1], x, 0.75f);
    x = ap_step(&rv->ap_in[2], x, 0.625f);
    x = ap_step(&rv->ap_in[3], x, 0.625f);

    // figure-8 tank: each branch takes the input + the OTHER branch's tail
    float tail_b = line_read(&rv->d_a2, rv->d_a2.len - 1) * decay;
    float tail_a = line_read(&rv->d_b2, rv->d_b2.len - 1) * decay;

    float a = ap_step(&rv->ap_a1, x + tail_a, 0.70f);
    line_push(&rv->d_a1, a);
    float ad = line_read(&rv->d_a1, rv->d_a1.len - 1);
    rv->damp_a += (1.0f - damp) * (ad - rv->damp_a);   // damping LPF
    a = ap_step(&rv->ap_a2, rv->damp_a * decay, 0.50f);
    line_push(&rv->d_a2, a);

    float b = ap_step(&rv->ap_b1, x + tail_b, 0.70f);
    line_push(&rv->d_b1, b);
    float bd = line_read(&rv->d_b1, rv->d_b1.len - 1);
    rv->damp_b += (1.0f - damp) * (bd - rv->damp_b);
    b = ap_step(&rv->ap_b2, rv->damp_b * decay, 0.50f);
    line_push(&rv->d_b2, b);

    // output taps (Dattorro accumulator table, size-scaled offsets)
    *yl = 0.6f * ( line_read(&rv->d_b1, s_tap[0])
                 + line_read(&rv->d_b1, s_tap[1])
                 - line_read(&rv->ap_b2, s_tap[2])
                 + line_read(&rv->d_b2, s_tap[3])
                 - line_read(&rv->d_a1, s_tap[4])
                 - line_read(&rv->ap_a2, s_tap[5])
                 - line_read(&rv->d_a2, s_tap[6]));
    *yr = 0.6f * ( line_read(&rv->d_a1, s_tap[7])
                 + line_read(&rv->d_a1, s_tap[8])
                 - line_read(&rv->ap_a2, s_tap[9])
                 + line_read(&rv->d_a2, s_tap[10])
                 - line_read(&rv->d_b1, s_tap[11])
                 - line_read(&rv->ap_b2, s_tap[12])
                 - line_read(&rv->d_b2, s_tap[13]));
}

// NaN guard (svf lesson: a NaN in a recursive network is permanent silence
// that looks like a hardware fault) — flush the tank if poisoned.
// COUNTED (audio_rv_report): the flush is an unchunked ~170 KB PSRAM memset in
// the AUDIO task, i.e. the very stall the chunked UI-context clear exists to
// avoid, so a firing here is audible. Chasing "a burst of noise 1-2 s into the
// first play after changing reverb type" (Arlo 2026-07-26) needs to know whether
// this ever fires, and guessing at that family has been wrong before.
static inline void tank_nan_guard(reverb_t *rv)
{
    if (!(rv->damp_a == rv->damp_a) || !(rv->damp_b == rv->damp_b)) {
        audio_rv_report(0, true);
        // Kill the recursive STATES immediately — cheap, and stops the poison
        // being written back into the lines on the next sample. The LINES
        // themselves hold NaN too, but zeroing 150 KB here is exactly the
        // 10-15 ms bus stall documented above, so it is handed to the
        // incremental flush instead and the reverb passes dry until it finishes
        // (~19 blocks, about 28 ms of no reverb — a gap, not a click).
        rv->in_lp = rv->damp_a = rv->damp_b = 0.0f;
        rv->shim_lp = rv->shim_dc = rv->shim_pos = 0.0f;
        rv->clear_off = 0;
    }
}

// One bounded chunk of the flush. 4 KB is ~0.25 ms of PSRAM writes, comfortably
// inside a 1.45 ms audio block (the UI-context clear uses 8 KB with a yield
// between; there is nothing to yield to here). Returns true while flushing, in
// which case the caller must leave the signal DRY — the lines are still poisoned.
static inline bool rv_flush_tick(reverb_t *rv)
{
    if (rv->clear_off < 0) return false;
    const size_t total = (size_t)RV_SLAB * sizeof(float);
    size_t off = (size_t)rv->clear_off;
    if (off >= total) {
        // done: restart the tank from a known-good zero state
        for (int i = 0; i < 4; i++) rv->ap_in[i].w = 0;
        rv->pre.w = rv->ap_a1.w = rv->d_a1.w = rv->ap_a2.w = rv->d_a2.w = 0;
        rv->ap_b1.w = rv->d_b1.w = rv->ap_b2.w = rv->d_b2.w = rv->shim.w = 0;
        rv->clear_off = -1;
        return false;
    }
    size_t n = (total - off < 4096) ? (total - off) : 4096;
    memset((uint8_t *)rv->slab + off, 0, n);
    rv->clear_off = (int)(off + n);
    return true;
}

void reverb_send_i32(reverb_t *rv, int32_t *dry, const int16_t *send, int frames)
{
    if (!rv->slab) { rv->cost_us = 0; return; }
    // as reverb_block_f: dry is untouched while the tank is being flushed
    if (rv_flush_tick(rv)) { rv->cost_us = 0; return; }
    if (rv->mode == RV_OFF) { rv->cost_us = 0; return; }
    int64_t t0 = esp_timer_get_time();
    const float decay = rv->decay, damp = rv->damp, bw = rv->in_bw;
    const float sg = rv->shim_gain;
    const float ret = rv->wet;          // RETURN level: dry stays full
    const bool meter = audio_fx_meters_armed();   // per-sample cost: opt in only
    float wpk = 0.0f;                   // WET-only peak, see audio.h
    for (int f = 0; f < frames; f++) {
        float sl = (float)send[f * 2], sr = (float)send[f * 2 + 1];
        float x = 0.5f * (sl + sr) * 0.6f;
        float yl, yr;
        tank_step(rv, x, decay, damp, bw, sg, &yl, &yr);
        if (meter) {
            float al = fabsf(yl), ar = fabsf(yr);
            if (al > wpk) wpk = al;
            if (ar > wpk) wpk = ar;
        }
        float fg = rv_fade_step(rv);
        float ol = (float)(dry[f * 2] >> 16)     + ret * fg * yl;
        float or_ = (float)(dry[f * 2 + 1] >> 16) + ret * fg * yr;
        if (ol > 32767.0f) ol = 32767.0f;
        if (ol < -32768.0f) ol = -32768.0f;
        if (or_ > 32767.0f) or_ = 32767.0f;
        if (or_ < -32768.0f) or_ = -32768.0f;
        dry[f * 2]     = ((int32_t)ol) << 16;
        dry[f * 2 + 1] = ((int32_t)or_) << 16;
    }
    if (meter) audio_rv_report((int)(wpk * 100.0f / 32767.0f), false);
    tank_nan_guard(rv);
    int us = (int)(esp_timer_get_time() - t0);
    rv->cost_us += (us - rv->cost_us) >> 3;
    audio_rv_cost(rv->cost_us);
}

void reverb_block_f(reverb_t *rv, float *buf, int frames)
{
    if (!rv->slab) { rv->cost_us = 0; return; }
    // flushing after a NaN: pass DRY (buf is already the dry signal) and chip
    // away at the tank a chunk at a time, rather than stalling the bus at once
    if (rv_flush_tick(rv)) { rv->cost_us = 0; return; }
    if (rv->mode == RV_OFF) { rv->cost_us = 0; return; }
    int64_t t0 = esp_timer_get_time();
    const float decay = rv->decay;
    const float damp = rv->damp;
    const float bw = rv->in_bw;
    const float wet = rv->wet;
    // equal-power dry/wet
    const float gd = cosf(wet * (float)M_PI_2);
    const float gw = sinf(wet * (float)M_PI_2);
    const float sg = rv->shim_gain;
    const bool meter = audio_fx_meters_armed();      // per-sample cost: opt in only
    float wpk = 0.0f;                                // WET-only peak, see audio.h

    for (int f = 0; f < frames; f++) {
        float dl = buf[f * 2];
        float dr = buf[f * 2 + 1];
        float x = 0.5f * (dl + dr) * 0.6f;           // mono tank feed, headroom

        float yl, yr;
        tank_step(rv, x, decay, damp, bw, sg, &yl, &yr);
        if (meter) {
            float al = fabsf(yl), ar = fabsf(yr);
            if (al > wpk) wpk = al;
            if (ar > wpk) wpk = ar;
        }

        // crossfade the whole wet/dry mix toward PURE DRY while fading, so a
        // mode change never dips the dry signal — only the reverb leaves
        float fg = rv_fade_step(rv);
        buf[f * 2]     = dl + fg * (gd * dl + gw * yl - dl);   // no clamp (chain soft-limits)
        buf[f * 2 + 1] = dr + fg * (gd * dr + gw * yr - dr);
    }

    if (meter) audio_rv_report((int)(wpk * 100.0f / 32767.0f), false);
    tank_nan_guard(rv);
    int us = (int)(esp_timer_get_time() - t0);
    rv->cost_us += (us - rv->cost_us) >> 3;
    audio_rv_cost(rv->cost_us);          // EMA, ~8-block settle
}

// int32<<16 wrapper (single-FX callers, e.g. Slicer): unpack -> worker -> clamp.
void reverb_block_i32(reverb_t *rv, int32_t *out, int frames)
{
    float buf[FX_SCRATCH_N];
    if (frames * 2 > FX_SCRATCH_N) frames = FX_SCRATCH_N / 2;
    fx_unpack_i32(out, buf, frames * 2);
    reverb_block_f(rv, buf, frames);
    for (int i = 0; i < frames * 2; i++) {
        int32_t s = (int32_t)buf[i];
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        out[i] = s << 16;
    }
}
