// Drum sampler engine — CV-triggered one-shot pads mixed to stereo.
// All-RAM playback (SD only at load time), polyphonic across all pads.
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "sample_ram.h"
#include "drum_priv.h"

dr_state_t dr;

// Cubic soft saturation, the classic: y = v - v^3/6.75 over |v| <= 1.5, hard
// ceiling past it. The knee is tangent to full scale at v = 1.5 (f(1.5) = 1,
// f'(1.5) = 0), so a driven pad leans into the ceiling instead of hitting it
// square. Below ~half scale it's essentially linear — unity playback is
// untouched (the mixer only calls this when a pad is actually driven).
static inline int soft_clip(int x)
{
    const float L = 32767.0f;
    float v = (float)x / L;
    if (v > 1.5f) v = 1.5f;
    else if (v < -1.5f) v = -1.5f;
    v = v - (v * v * v) / 6.75f;
    return (int)(v * L);
}

// where a hit starts: the head the pad is told to skip (knob7 clockwise, in
// DR_CW_START mode). Clamped well inside the sample so a hit is never silence.
static uint32_t layer_start(const dr_pad_t *p, const dr_layer_t *L)
{
    if (!p->start_off || !L->len) return 0;
    uint32_t st = (uint32_t)(((uint64_t)p->start_off * L->len) >> 8);
    if (st >= L->len - 64) st = L->len - 64;
    return st;
}

// Trigger a LAYER of a pad. The two layers share one voice, so a hit on either
// interrupts whatever is sounding: `retrig` no longer means "restart me", it
// means "fade what's playing, then start next_layer" — which may be the other
// buffer entirely. The fade is slower for a cross-layer choke: at 0.7 ms one
// sound cutting another off reads as a gate click.
static void trigger_pad(dr_pad_t *p, int ly, uint8_t vel)
{
    dr_layer_t *L = &p->ly[ly];
    if (!p->enabled || !L->buf || L->len == 0) return;
    if (p->playing) {
        bool cross = (p->cur != (uint8_t)ly);
        p->vel_next   = vel;
        p->next_layer = (uint8_t)ly;
        if (!p->retrig) p->fade = 256;    // don't restart an in-flight fade, or a
                                          // machine-gun roll would never land it
        p->fade_step  = cross ? DR_CHOKE_STEP : DR_RETRIG_STEP;
        p->retrig = true;
    } else {
        p->cur    = (uint8_t)ly;
        p->pos    = layer_start(p, L);
        p->vel    = vel;
        p->retrig = false;
        p->playing = true;
    }
    p->hit = true;
    p->hit_layer = (uint8_t)ly;
}

// Everything the mixer's inner loop treats as invariant — and every one of these
// belongs to the LAYER, not the pad. The layer can flip mid-block (a choke lands
// when the fade completes), so this MUST be re-run at the flip: reading a shorter
// B with A's length walks the cursor off the end of B's PSRAM buffer.
typedef struct {
    const int16_t *buf;
    uint32_t len, st, ll, df, af;
} dr_voice_t;

static void voice_setup(dr_pad_t *p, dr_voice_t *v)
{
    dr_layer_t *L = &p->ly[p->cur];
    v->buf = L->buf;
    v->len = L->len;
    v->df  = (uint32_t)p->decay_ms * 441 / 10;    // decay length in frames
    v->af  = (uint32_t)p->attack_ms * 441 / 10;   // attack length in frames
    v->st  = layer_start(p, L);                   // hits begin here
    // stutter loop: the read wraps inside [st, st+ll) while the pad's LIFE is
    // still the whole sample — so the roll ends when the one-shot would have,
    // instead of droning. A loop longer than what's left is no loop at all.
    v->ll  = (uint32_t)p->loop_ms * 441 / 10;
    if (!v->len || v->ll >= v->len - v->st) v->ll = 0;
}

static esp_err_t drum_start(void)
{
    memset(&dr, 0, sizeof(dr));
    // Buffers are allocated LAZILY, at load time (drum_load_layer). 8 pads x 2
    // layers x 176 KB would be 2.8 MB claimed up front — more than anything on
    // this board has proven, and most of it for pads that stay empty. An empty
    // kit now costs nothing, and the 16th load fails soft instead of crashing.
    for (int i = 0; i < DR_PADS; i++) {
        dr.pad[i].enabled = true;
        dr.pad[i].level = 255;
        dr.pad[i].pan = 128;
        for (int l = 0; l < DR_LAYERS; l++) {
            // A fires from the pad's own CV; B fires from NOTHING until it is
            // routed — sharing A's channel would make B choke A on every hit,
            // which reads as a bug rather than a feature
            dr.pad[i].ly[l].trig_src = (l == 0) ? i : DR_SRC_NONE;
            dr.pad[i].ly[l].base = 4095;   // floor tracker converges down on first reads
        }
    }
    dr.cv_mod = true;       // knob6/knob7 perform the selected pad
    dr.flt_box = true;      // the master filter box is reachable from Live
    dr.flt_on = false;
    dr.flt_cv = 2048;       // centre = bypass
    dr.flt_res_cv = 0;      // clean
    dr.flt_q = DR_Q_CLEAN;
    dr.flt_ref_f = -1;      // pickup: seize the reference on the first block
    dr.flt_ref_q = -1;
    svf_reset(&dr.flt_l);
    svf_reset(&dr.flt_r);
    dr.sens = 1;            // Med
    dr.sel_src[0] = 5;      // knob6/knob7 — the two fully-good CV channels
    dr.sel_src[1] = 6;
    dr.prev_trig = 0x03;    // gates idle high; no phantom edge on boot
    audio_status_set_voices("drums", "");
    return ESP_OK;
}

static void drum_stop(void)
{
    reverb_free(&dr.rv);
    for (int i = 0; i < DR_PADS; i++)
        for (int l = 0; l < DR_LAYERS; l++) {
            free(dr.pad[i].ly[l].buf);
            dr.pad[i].ly[l].buf = NULL;
        }
}

static void drum_process(int32_t out[MACHINE_BLOCK],
                         const int32_t in[MACHINE_BLOCK],
                         const machine_io_t *io)
{
    // (no "is pad 0 loaded" guard any more: with lazy allocation an empty pad 0 is
    // normal. Every read is gated on its own layer's buf/len.)

    // ---- CV6/CV7 perform the SELECTED PAD (move-to-take-over; see drum_priv.h) --
    // A channel already spoken for as a CV-select selector is left alone — the
    // selectors default to exactly these two channels, and one knob can't both
    // address a pad and set its level. In Direct mode the selectors don't exist,
    // so nothing blocks the knobs there.
    bool lv_free = !(dr.cv_select && (dr.sel_src[0] == DR_MOD_LEVEL_CV ||
                                      dr.sel_src[1] == DR_MOD_LEVEL_CV));
    bool dc_free = !(dr.cv_select && (dr.sel_src[0] == DR_MOD_DECAY_CV ||
                                      dr.sel_src[1] == DR_MOD_DECAY_CV));
    int lv = io->cv[DR_MOD_LEVEL_CV], dc = io->cv[DR_MOD_DECAY_CV];
    if (!dr.knob_seen) {                          // first block: adopt, don't apply
        dr.knob_last[0] = lv;
        dr.knob_last[1] = dc;
        dr.knob_seen = true;
    }

    // the knobs aim at ONE thing: the selected pad, or the filter box
    if (dr.cv_mod && dr.flt_box && dr.sel_filter) {
        // grab-then-track: nothing moves until the knob leaves where it sat when
        // the box was selected, and then it tracks continuously — a sweep can't
        // be stepped like a pad parameter
        if (dr.flt_ref_f < 0) dr.flt_ref_f = lv;
        if (dr.flt_ref_q < 0) dr.flt_ref_q = dc;
        int df = lv - dr.flt_ref_f, dq = dc - dr.flt_ref_q;
        if (df < 0) df = -df;
        if (dq < 0) dq = -dq;
        if (lv_free) {
            if (!dr.flt_take_f && df > DR_MOD_MOVE) dr.flt_take_f = true;
            if (dr.flt_take_f) dr.flt_cv = lv;
        }
        if (dc_free) {
            if (!dr.flt_take_q && dq > DR_MOD_MOVE) dr.flt_take_q = true;
            if (dr.flt_take_q) dr.flt_res_cv = dc;
        }
    } else if (dr.cv_mod) {
        int sel = dr.sel_pad;
        if (sel < 0 || sel >= DR_PADS) sel = 0;
        dr_pad_t *sp = &dr.pad[sel];
        int dlv = lv - dr.knob_last[0], ddc = dc - dr.knob_last[1];
        if (dlv < 0) dlv = -dlv;
        if (ddc < 0) ddc = -ddc;
        if (lv_free && dlv > DR_MOD_MOVE) {
            dr.knob_last[0] = lv;
            // 12 o'clock = 100 % (unity), CCW fades to silence, CW drives the
            // pad into the soft clipper (Arlo)
            if (lv <= 2048)
                sp->level = (uint16_t)((uint32_t)lv * DR_LEVEL_UNITY / 2048);
            else
                sp->level = (uint16_t)(DR_LEVEL_UNITY +
                                       (uint32_t)(lv - 2048) *
                                       (DR_LEVEL_MAX - DR_LEVEL_UNITY) / 2047);
        }
        if (dc_free && ddc > DR_MOD_MOVE) {
            dr.knob_last[1] = dc;
            // Both knobs are NEUTRAL at 12 o'clock (Arlo). Counter-clockwise from
            // noon chokes the decay — 1.5 s just under centre down to 20 ms hard
            // left. Clockwise drives whichever target the pad selects (attack or
            // start offset). At centre the pad is simply the untouched sample, so
            // an external CV that never swings below half (a 0-5 V source on a
            // bipolar input sits there) leaves it alone instead of gating it off.
            if (dc < 2048) {
                sp->decay_ms = (uint16_t)(20 + (uint32_t)dc * 1480 / 2048);
                sp->attack_ms = 0;
                sp->start_off = 0;
                sp->loop_ms = 0;
            } else {
                uint32_t cw = (uint32_t)(dc - 2048);          // 0..2047 above noon
                sp->decay_ms = 0;                             // full sample again
                sp->attack_ms = 0;
                sp->start_off = 0;
                sp->loop_ms = 0;
                if (sp->cw_mode == DR_CW_NONE) {
                    /* nothing: the pad is simply the untouched sample up here */
                } else if (sp->cw_mode == DR_CW_START) {
                    sp->start_off = (uint8_t)(cw * DR_START_MAX / 2047);
                } else if (sp->cw_mode == DR_CW_ATTACK) {
                    sp->attack_ms = (uint16_t)(cw * DR_ATTACK_MAX / 2047);
                } else if (sp->cw_mode == DR_CW_LOOP && cw > 40) {  // dead spot at noon
                    // further clockwise = SHORTER loop (Arlo): a roll that tightens
                    // into a buzz as you turn
                    uint32_t span = DR_LOOP_MAX_MS - DR_LOOP_MIN_MS;
                    sp->loop_ms = (uint16_t)(DR_LOOP_MAX_MS - cw * span / 2047);
                }
            }
        }
    }

    // ---- master filter coefficients (once per block; the deck's mapping) ------
    {
        int fcv = dr.flt_cv;
        int mode = 0;
        float fc = 0;
        if (fcv < 2048 - 150) {                  // LP zone: sweeps DOWN to the left
            mode = 1;
            float t = (float)fcv / (2048.0f - 150.0f);
            fc = 80.0f * powf(150.0f, t);                    // 80 Hz .. 12 kHz
        } else if (fcv > 2048 + 150) {           // HP zone: sweeps UP to the right
            mode = 2;
            float t = (float)(fcv - 2048 - 150) / (4095.0f - 2048.0f - 150.0f);
            fc = 30.0f * powf(200.0f, t);                    // 30 Hz .. 6 kHz
        }
        dr.flt_mode = mode;
        float f_t = mode ? svf_coef(fc, (float)DR_RATE, DR_FLT_FMAX) : 0.0f;
        dr.flt_f += 0.2f * (f_t - dr.flt_f);     // slewed: no zipper on a fast sweep
        // resonance, and the reason the deck could skip this: a Chamberlin with
        // low damping AND a high coefficient self-oscillates, so the damping floor
        // has to rise with the cutoff
        float q_t = svf_damp((float)dr.flt_res_cv / 4095.0f, DR_Q_SQUELCH, DR_Q_CLEAN);
        float qfloor = DR_Q_SQUELCH + 0.8f * (dr.flt_f > 0.85f ? (dr.flt_f - 0.85f) : 0.0f);
        if (q_t < qfloor) q_t = qfloor;
        dr.flt_q += 0.2f * (q_t - dr.flt_q);     // a jumped q clicks: slew it too
    }

    // ---- triggers ----
    if (!dr.cv_select) {
        // Direct: each pad watches its routed CV input through a Schmitt
        // detector referenced to that source's tracked floor
        static const int fire_d[3] = {1500, 1100, 700};
        static const int arm_d[3]  = {600, 450, 300};
        int sens = dr.sens;
        if (sens < 0) sens = 0;
        if (sens > 2) sens = 2;
        for (int ch = 0; ch < DR_PADS; ch++) {
            dr_pad_t *p = &dr.pad[ch];
            int nly = p->layered ? DR_LAYERS : 1;
            for (int l = 0; l < nly; l++) {
                dr_layer_t *L = &p->ly[l];
                if (!L->buf || !L->len || L->trig_src == DR_SRC_NONE) continue;
                int v = io->cv[L->trig_src & 7];
                if (v < L->base) L->base = v;           // dips pull the floor down instantly
                else if (L->base < 4095) L->base++;     // ~690/s upward drift back
                if (!L->armed) {
                    if (v < L->base + arm_d[sens]) L->armed = true;
                } else if (v >= L->base + fire_d[sens]) {
                    L->armed = false;
                    int vel = (v - L->base) >> 3;       // velocity from swing above floor
                    if (vel > 255) vel = 255;
                    trigger_pad(p, l, dr.velocity ? (uint8_t)vel : 255);
                }
            }
        }
        dr.prev_trig = io->trig_level;   // keep edge state fresh across mode switches
    } else {
        // CV-select: TRIG1/2 falling edge fires the pad its selector CV addresses
        uint8_t pressed = dr.prev_trig & (~io->trig_level) & 0x03;
        dr.prev_trig = io->trig_level;
        for (int t = 0; t < 2; t++) {
            if (!(pressed & (1 << t))) continue;
            int src = dr.sel_src[t];
            if (src < 0) continue;              // no selector: this gate fires nothing
            uint16_t sv = io->cv[src & 7];
            int idx = (int)((uint32_t)sv * (uint32_t)DR_PADS / 4096);
            if (idx >= DR_PADS) idx = DR_PADS - 1;
            // with layers on, the two gates address the two LAYERS of whichever pad
            // their selector points at — TR1 fires A, TR2 fires B. (Quantizing the
            // selector over n_pads*2 slots instead would halve the CV window per
            // slot to ~2.5 %, which these inputs cannot play.)
            int ly = (dr.pad[idx].layered && t == 1) ? 1 : 0;
            trigger_pad(&dr.pad[idx], ly, 255);
        }
    }

    // ---- mix active pads (mono buffers -> stereo, linear pan, one-shot) ----
    int frames = MACHINE_BLOCK / 2;
    int32_t accL[MACHINE_BLOCK / 2], accR[MACHINE_BLOCK / 2];
    memset(accL, 0, sizeof(accL));
    memset(accR, 0, sizeof(accR));
    bool any = false;
    for (int i = 0; i < DR_PADS; i++) {
        dr_pad_t *p = &dr.pad[i];
        if (!p->playing) continue;
        dr_voice_t v;
        voice_setup(p, &v);
        if (!v.buf || !v.len) { p->playing = false; continue; }
        any = true;
        uint32_t len = v.len, df = v.df, af = v.af, st = v.st, ll = v.ll;
        for (int f = 0; f < frames; f++) {
            if (p->retrig) {
                // fade the sounding voice out, then start next_layer — the same
                // buffer on a retrigger, the OTHER one on a choke
                p->fade -= p->fade_step;
                if (p->fade <= 0) {
                    p->retrig = false;
                    p->cur = p->next_layer;
                    // the flip lands HERE, mid-block, and len/st/ll all belong to
                    // the layer: refresh them or a shorter B is read with A's
                    // length and the cursor walks off the end of its buffer
                    voice_setup(p, &v);
                    if (!v.buf || !v.len) { p->playing = false; break; }
                    len = v.len; df = v.df; af = v.af; st = v.st; ll = v.ll;
                    p->pos = st;
                    p->vel = p->vel_next;
                    continue;
                }
            }
            uint32_t pos = p->pos;
            // an INF loop outlives the sample: pos keeps counting past len (the
            // read still wraps inside the loop window), so it can't be the thing
            // that ends the voice — only a retrigger does
            bool inf = (ll && p->loop_reps == DR_REPS_INF);
            if (!inf && pos >= len) { p->playing = false; break; }
            int env;
            if (p->retrig) {
                env = p->fade;
            } else {
                // the envelopes run from the START of the hit, not from frame 0 of
                // the buffer — a skipped-into pad still gets its attack and decay.
                // pos can legitimately sit BELOW st (the knob moved the start point
                // while this voice was already running): an unsigned pos - st there
                // wraps to ~4e9 and indexes far outside the buffer.
                uint32_t el = (pos > st) ? pos - st : 0;
                env = 256;
                if (af) {                                   // knob7 CW: fade the hit in
                    if (el < af) env = (int)(((uint64_t)el << 8) / af);
                } else if (el < 64) {
                    env = (int)el << 2;                     // ~1.5 ms declick attack
                }
                if (!inf) {                                 // (an INF loop has no tail:
                    uint32_t rem = len - pos;               //  pos runs past len)
                    if (rem < 256 && (int)rem < env) env = (int)rem;   // ~6 ms tail
                }
                if (df) {                                   // optional decay envelope
                    if (el >= df) { p->playing = false; break; }
                    int denv = (int)(((df - el) << 8) / df);
                    if (denv < env) env = denv;
                }
                if (ll) {
                    // the retrig cap: N repeats, then the pad is done (a 4-rep
                    // 60 ms loop is a fill, not a stutter that outstays the beat)
                    if (p->loop_reps && p->loop_reps != DR_REPS_INF &&
                        el >= (uint32_t)p->loop_reps * ll) {
                        p->playing = false;
                        break;
                    }
                    // every repeat gets its own ~0.7 ms in/out ramp: the loop seam
                    // lands on an arbitrary sample value and would tick otherwise
                    uint32_t ph = el % ll;
                    uint32_t left = ll - ph;
                    int lenv = 256;
                    if (ph < 32) lenv = (int)ph << 3;
                    if (left < 32 && (int)left << 3 < lenv) lenv = (int)left << 3;
                    if (lenv < env) env = lenv;
                }
            }
            // the loop wraps the READ inside the window; pos keeps counting, so the
            // pad still dies at the end of the sample's natural length. Same
            // underflow guard as above: st can move under a running voice.
            uint32_t lel = (pos > st) ? pos - st : 0;
            uint32_t idx = ll ? st + (lel % ll) : pos;
            int s = (v.buf[idx] * env) >> 8;
            // gain: 255 = unity, above that the pad is driven. Clip SOFTLY —
            // letting a 4x sample slam into the int16 clamp is fuzz, not drive.
            int g = ((int)p->level * (int)p->vel) >> 8;      // 0..DR_LEVEL_MAX
            int x = (s * g) >> 8;
            if (p->level > DR_LEVEL_UNITY) x = soft_clip(x);
            accL[f] += (x * (255 - p->pan)) >> 8;
            accR[f] += (x * p->pan) >> 8;
            p->pos = pos + 1;
        }
    }
    // ---- master filter + output ------------------------------------------------
    // The filter must keep running after the last pad dies: its ring is still
    // decaying, and returning early would chop the tail off.
    bool flt_live = dr.flt_box && dr.flt_on;
    bool rv_live = (dr.rv.mode != RV_OFF) && dr.rv.slab;
    // the filter AND the reverb must keep running after the last pad dies:
    // their rings/tails are still decaying
    if (!any && !flt_live && !rv_live) return;

    // a NaN in an SVF is PERMANENT silence — it would read as dead hardware
    if (!(fabsf(dr.flt_l.lp) < 1e9f) || !(fabsf(dr.flt_r.lp) < 1e9f)) {
        svf_reset(&dr.flt_l);
        svf_reset(&dr.flt_r);
    }

    for (int f = 0; f < frames; f++) {
        int32_t li = accL[f], ri = accR[f];
        if (li > 32767) li = 32767; else if (li < -32768) li = -32768;
        if (ri > 32767) ri = 32767; else if (ri < -32768) ri = -32768;
        float l = (float)li, r = (float)ri;
        if (flt_live && dr.flt_mode) {
            float lo, hi;
            svf_step(&dr.flt_l, l, dr.flt_f, dr.flt_q, &lo, NULL, &hi);
            l = (dr.flt_mode == 1) ? lo : hi;
            svf_step(&dr.flt_r, r, dr.flt_f, dr.flt_q, &lo, NULL, &hi);
            r = (dr.flt_mode == 1) ? lo : hi;
        } else {
            svf_park(&dr.flt_l, l);        // bypass parks the state ON the signal,
            svf_park(&dr.flt_r, r);        // so re-engaging can't thump
        }
        int32_t lo32 = (int32_t)l, ro32 = (int32_t)r;   // resonance overshoots: clamp
        if (lo32 > 32767) lo32 = 32767; else if (lo32 < -32768) lo32 = -32768;
        if (ro32 > 32767) ro32 = 32767; else if (ro32 < -32768) ro32 = -32768;
        out[f * 2]     = lo32 << 16;
        out[f * 2 + 1] = ro32 << 16;
    }
    if (rv_live) reverb_block_i32(&dr.rv, out, frames);   // post-filter, master
}

// ---- sample I/O (UI task) ---------------------------------------------------
// pad-cell thumbnail: peak per column straight out of the RAM buffer. No SD
// pass and no yield needed — 48 columns over a 2 s mono buffer at most.
static void drum_build_wf(dr_layer_t *L)
{
    L->wf_valid = false;
    if (!L->len || !L->buf) return;
    for (int c = 0; c < DR_WF_W; c++) {
        uint32_t a = (uint32_t)((uint64_t)c * L->len / DR_WF_W);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * L->len / DR_WF_W);
        if (b <= a) b = a + 1;
        if (b > L->len) b = L->len;
        int peak = 0;
        for (uint32_t k = a; k < b; k++) {
            int s = L->buf[k];
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        L->wf[c] = (uint8_t)(peak >> 7);      // 0..32767 -> 0..255, sampler scale
    }
    L->wf_valid = true;
}

int drum_load_layer(int pad, int ly, const char *name)
{
    if (pad < 0 || pad >= DR_PADS || ly < 0 || ly >= DR_LAYERS) return -1;
    dr_pad_t *p = &dr.pad[pad];
    dr_layer_t *L = &p->ly[ly];

    // silence this pad's voice before the buffer under it changes
    p->playing = false;
    p->retrig = false;
    L->len = 0;
    L->wf_valid = false;

    if (!L->buf) {      // lazy: a layer costs PSRAM only once something is in it
        L->buf = heap_caps_malloc((size_t)DR_MAX_FRAMES * sizeof(int16_t),
                                  MALLOC_CAP_SPIRAM);
        if (!L->buf) {
            ESP_LOGE("DRUM", "PSRAM full: pad %d layer %c (%u KB free)",
                     pad + 1, 'A' + ly,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            L->sample[0] = 0;
            return -1;
        }
    }
    uint32_t n = sample_load(name, L->buf, DR_MAX_FRAMES, true);   // mono
    if (n == 0) { L->sample[0] = 0; return -1; }
    strncpy(L->sample, name, sizeof(L->sample) - 1);
    L->sample[sizeof(L->sample) - 1] = 0;
    L->len = n;
    drum_build_wf(L);
    ESP_LOGI("DRUM", "pad %d%c: %s (%lu frames, %u KB PSRAM free)",
             pad + 1, 'A' + ly, name, (unsigned long)n,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return 0;
}

int drum_load_pad(int pad, const char *name) { return drum_load_layer(pad, 0, name); }

void drum_clear_layer(int pad, int ly)
{
    if (pad < 0 || pad >= DR_PADS || ly < 0 || ly >= DR_LAYERS) return;
    dr_pad_t *p = &dr.pad[pad];
    dr_layer_t *L = &p->ly[ly];
    // the engine gates every read on len, and never holds buf across a block —
    // so: stop it reading, give it a tick to finish the block in flight, THEN
    // free (the looper's swap protocol; one tick is 10 ms vs a 1.45 ms block)
    p->playing = false;
    p->retrig = false;
    L->len = 0;
    L->sample[0] = 0;
    L->wf_valid = false;
    vTaskDelay(1);
    free(L->buf);
    L->buf = NULL;
}

void drum_clear_pad(int pad) { drum_clear_layer(pad, 0); }

// ---- preset -----------------------------------------------------------------
static cJSON *drum_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "cvsel", dr.cv_select);
    cJSON_AddBoolToObject(o, "vel", dr.velocity);
    cJSON_AddBoolToObject(o, "cvmod", dr.cv_mod);
    cJSON_AddNumberToObject(o, "rv", dr.rv.mode);     // master reverb mode
    cJSON_AddNumberToObject(o, "rvmx", (int)(dr.rv.wet * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "rvus", dr.rv.cost_us); // info only (cost meter)
    cJSON_AddBoolToObject(o, "flt", dr.flt_box);      // master filter box exists
    cJSON_AddBoolToObject(o, "flton", dr.flt_on);     // ...and is engaged
    cJSON_AddNumberToObject(o, "fcv", dr.flt_cv);     // sweep + resonance knob
    cJSON_AddNumberToObject(o, "fres", dr.flt_res_cv);// positions (knob pickup
                                                      // holds them until moved)
    cJSON_AddNumberToObject(o, "sens", dr.sens);
    cJSON_AddNumberToObject(o, "sel0", dr.sel_src[0]);
    cJSON_AddNumberToObject(o, "sel1", dr.sel_src[1]);
    cJSON *pads = cJSON_AddArrayToObject(o, "pad");
    for (int i = 0; i < DR_PADS; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "s", dr.pad[i].ly[0].sample);
        cJSON_AddNumberToObject(p, "lvl", dr.pad[i].level);
        cJSON_AddNumberToObject(p, "pan", dr.pad[i].pan);
        cJSON_AddNumberToObject(p, "dec", dr.pad[i].decay_ms);
        cJSON_AddBoolToObject(p, "en", dr.pad[i].enabled);
        cJSON_AddNumberToObject(p, "src", dr.pad[i].ly[0].trig_src + 1);  // CV number, 1-based
        cJSON_AddBoolToObject(p, "lay", dr.pad[i].layered);               // B layer on?
        cJSON_AddStringToObject(p, "s2", dr.pad[i].ly[1].sample);         // the B layer
        cJSON_AddNumberToObject(p, "src2", dr.pad[i].ly[1].trig_src + 1); // (0 = none)
        cJSON_AddNumberToObject(p, "cw", dr.pad[i].cw_mode);         // knob7 clockwise target
        cJSON_AddNumberToObject(p, "reps", dr.pad[i].loop_reps);     // retrig cap (255 = INF)
        cJSON_AddItemToArray(pads, p);
    }
    return o;
}

static void drum_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    // "pads" (4-vs-8) is gone; an old preset carrying it is simply ignored
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cvsel"))) dr.cv_select = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "vel")))   dr.velocity = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cvmod")))  dr.cv_mod = cJSON_IsTrue(j);
    // "lay" used to be a global flag; if an old preset carries it, fan it out to
    // every pad (the per-pad "lay" below then overrides where present)
    bool lay_all = false;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lay"))) lay_all = cJSON_IsTrue(j);
    if (lay_all) for (int k = 0; k < DR_PADS; k++) dr.pad[k].layered = true;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rv")) && cJSON_IsNumber(j)) {
        int m = j->valueint;
        if (m < 0 || m >= RV_N_MODES) m = RV_OFF;
        // lazy slab: only a non-OFF mode costs PSRAM; a failed alloc fails soft
        if (m != RV_OFF && !dr.rv.slab && reverb_init(&dr.rv) != ESP_OK) m = RV_OFF;
        reverb_set_mode(&dr.rv, m);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rvmx")) && cJSON_IsNumber(j))
        reverb_set_mix(&dr.rv, (float)j->valueint / 100.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "flt")))    dr.flt_box = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "flton")))  dr.flt_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fcv")) && cJSON_IsNumber(j)) {
        int f = j->valueint;
        dr.flt_cv = (f < 0) ? 0 : (f > 4095 ? 4095 : f);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fres")) && cJSON_IsNumber(j)) {
        int f = j->valueint;
        dr.flt_res_cv = (f < 0) ? 0 : (f > 4095 ? 4095 : f);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sens")) && cJSON_IsNumber(j)) {
        dr.sens = j->valueint;
        if (dr.sens < 0) dr.sens = 0;
        if (dr.sens > 2) dr.sens = 2;
    }
    // -1 = none is a legal selector now: clamp, don't mask (a mask turned it into CV8)
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sel0")) && cJSON_IsNumber(j))
        dr.sel_src[0] = (j->valueint < 0) ? -1 : (j->valueint & 7);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sel1")) && cJSON_IsNumber(j))
        dr.sel_src[1] = (j->valueint < 0) ? -1 : (j->valueint & 7);
    cJSON *pads = cJSON_GetObjectItemCaseSensitive(node, "pad");
    if (cJSON_IsArray(pads)) {
        int i = 0;
        cJSON *p;
        cJSON_ArrayForEach(p, pads) {
            if (i >= DR_PADS) break;
            // level is uint16 now (unity 255, up to DR_LEVEL_MAX driven): a uint8
            // cast here would silently pin every driven pad back to unity
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "lvl")) && cJSON_IsNumber(j)) {
                int lv = j->valueint;
                if (lv < 0) lv = 0;
                if (lv > DR_LEVEL_MAX) lv = DR_LEVEL_MAX;
                dr.pad[i].level = (uint16_t)lv;
            }
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "pan")) && cJSON_IsNumber(j)) dr.pad[i].pan = (uint8_t)j->valueint;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "dec")) && cJSON_IsNumber(j)) {
                int d = j->valueint;
                if (d < 0) d = 0;
                if (d > 5000) d = 5000;
                dr.pad[i].decay_ms = (uint16_t)d;
            }
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "en")))                       dr.pad[i].enabled = cJSON_IsTrue(j);
            // src is 1-based; 0 (or absent) means NONE, which is how a -1 round-trips
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "src")) && cJSON_IsNumber(j))
                dr.pad[i].ly[0].trig_src = (j->valueint <= 0) ? DR_SRC_NONE
                                                              : ((j->valueint - 1) & 7);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "lay")))
                dr.pad[i].layered = cJSON_IsTrue(j);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "src2")) && cJSON_IsNumber(j))
                dr.pad[i].ly[1].trig_src = (j->valueint <= 0) ? DR_SRC_NONE
                                                              : ((j->valueint - 1) & 7);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "cw")) && cJSON_IsNumber(j))
                dr.pad[i].cw_mode = (uint8_t)(j->valueint % DR_CW_MODES);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "reps")) && cJSON_IsNumber(j)) {
                int rp = j->valueint;
                if (rp < 0) rp = 0;
                if (rp > DR_REPS_INF) rp = DR_REPS_INF;
                dr.pad[i].loop_reps = (uint8_t)rp;
            }
            // reload the remembered samples. An old preset has no "s2" — absent
            // simply means the pad has no B layer, which is today's behaviour.
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "s")) && cJSON_IsString(j) && j->valuestring[0])
                drum_load_layer(i, 0, j->valuestring);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "s2")) && cJSON_IsString(j) && j->valuestring[0])
                drum_load_layer(i, 1, j->valuestring);   // fails soft if PSRAM is gone
            i++;
        }
    }
}

extern const machine_ui_t drum_menu_ui;

const machine_t machine_drumsampler = {
    .name = "Drums",
    .start = drum_start,
    .stop = drum_stop,
    .process = drum_process,
    .preset_save = drum_preset_save,
    .preset_load = drum_preset_load,
    .ui = &drum_menu_ui,
};
