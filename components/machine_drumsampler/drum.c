// Drum sampler engine — CV-triggered one-shot pads mixed to stereo.
// All-RAM playback (SD only at load time), polyphonic across all pads.
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "cvsmooth.h"
#include "audio.h"
#include "sample_ram.h"
#include "drum_priv.h"

dr_state_t dr;
fxrack_t dr_rk;    // FX rack pointer-view (initialized in drum_start)

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

// per-semitone playback step in Q12 (4096 = native), -12..+12: index by
// pitch_semi + 12. A LUT because the mixer must not call powf per voice.
static const uint16_t dr_pitch_q12[25] = {
    2048, 2170, 2299, 2435, 2580, 2734, 2896, 3069, 3251, 3444, 3649, 3866,
    4096, 4340, 4598, 4871, 5161, 5468, 5793, 6137, 6502, 6889, 7298, 7732,
    8192,
};

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
        p->pos_fr = 0;
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
    uint32_t step;                // Q12 cursor advance per output frame (pitch)
} dr_voice_t;

static void voice_setup(dr_pad_t *p, dr_voice_t *v)
{
    dr_layer_t *L = &p->ly[p->cur];
    v->buf = L->buf;
    v->len = L->len;
    int pi = p->pitch_semi + p->pitch_cv;   // base + live CV offset
    if (pi < -12) pi = -12;
    if (pi > 12) pi = 12;
    v->step = dr_pitch_q12[pi + 12];
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
        dr.pad[i].fx_on = true;   // wet by default: FX1/FX2 start Off, so this
                                  // is silent until an effect is chosen — and a
                                  // legacy master-delay preset keeps its sound
        dr.pad[i].pitch_src = DR_SRC_NONE;
        dr.pad[i].pitch_floor = 4095;      // V/oct floor converges down on first reads
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
    fxfilter_init(&dr.filt);
    fxfilter_init(&dr.band);
    dr.fx_slot[0] = dr.fx_slot[1] = FXK_OFF;
    dr_rk = (fxrack_t){ .od = &dr.od, .flg = &dr.flg, .trem = &dr.trem, .dly = &dr.dly,
                        .filt = &dr.filt, .band = &dr.band, .rv = &dr.rv, .slot = dr.fx_slot };
    audio_status_set_voices("drums", "");
    return ESP_OK;
}

static void drum_stop(void)
{
    reverb_free(&dr.rv);
    fxdelay_free(&dr.dly);
    flanger_free(&dr.flg);
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
    // conditioned CV (cvsmooth.h): every knob read below goes through the median
    static cvmed_t s_dmed[8];
    int cvm[8];
    for (int k = 0; k < 8; k++) cvm[k] = cvmed_step(&s_dmed[k], io->cv[k]);

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
    // MEDIAN-OF-5 on the performance knobs (cvsmooth.h). The grab-then-track guard
    // below rejects small JITTER but PASSES a big excursion — so a lone ADC outlier
    // (a steady ~1221 reporting ONE sample of 4) both falsely SEIZES the knob and
    // slams the value: the pad level jumps, and the master filter's cutoff drops to
    // the floor for a block. A median rejects the outlier outright.
    int lv = cvm[DR_MOD_LEVEL_CV], dc = cvm[DR_MOD_DECAY_CV];
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
        // autosave: the sweep tracks CONTINUOUSLY once seized, so flag only a
        // real excursion since the last flag — not the per-block jitter — or
        // the dirty poll's backstop would fire forever
        static int s_df_flag = -1, s_dq_flag = -1;
        if (dr.flt_take_f) {
            int d2 = dr.flt_cv - s_df_flag;
            if (d2 < 0) d2 = -d2;
            if (s_df_flag < 0 || d2 > DR_MOD_MOVE) { s_df_flag = dr.flt_cv; machine_state_dirty(); }
        }
        if (dr.flt_take_q) {
            int d2 = dr.flt_res_cv - s_dq_flag;
            if (d2 < 0) d2 = -d2;
            if (s_dq_flag < 0 || d2 > DR_MOD_MOVE) { s_dq_flag = dr.flt_res_cv; machine_state_dirty(); }
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
            machine_state_dirty();          // knob edits never reach the UI queue
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
            machine_state_dirty();
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
                // pitch is knob-owned ONLY in DR_CW_PITCH mode (it has its own
                // Pads row, unlike the other CW targets) — don't wipe a
                // row-set pitch from a decay gesture in another mode
                if (sp->cw_mode == DR_CW_PITCH) sp->pitch_semi = 0;
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
                } else if (sp->cw_mode == DR_CW_PITCH) {
                    // tune DOWN from native, -12 at full clockwise (tuning up
                    // is on the Pads row; down is the performance move)
                    sp->pitch_semi = (int8_t)-((int)cw * 12 / 2047);
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

    // ---- per-pad PITCH CV -> quantized semitone offset (see drum_priv.h) ----
    // computed every block so a held/ringing pad retunes live (voice_setup
    // refreshes the step per block)
    for (int i = 0; i < DR_PADS; i++) {
        dr_pad_t *p = &dr.pad[i];
        int srcp = p->pitch_src;
        if (srcp < 0) { p->pitch_cv = 0; continue; }
        int c = cvm[srcp & 7];
        int semi;
        if (p->pitch_mode == DR_PCV_VOCT) {
            // root at the channel's idle: follow the floor down, drift back up
            if (c < p->pitch_floor) p->pitch_floor = c;
            else if (p->pitch_floor < 4095) p->pitch_floor++;
            semi = (c - p->pitch_floor + DR_PCV_CTS_SEMI / 2) / DR_PCV_CTS_SEMI;
            if (semi > 12) semi = 12;
        } else {                       // +/-: bipolar around mid-scale
            semi = ((c - 2048) * 12) / 2048;
            if (semi < -12) semi = -12;
            if (semi > 12) semi = 12;
        }
        p->pitch_cv = (int8_t)semi;
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
                int v = io->cv[L->trig_src & 7];   // RAW: a median would swallow a short gate
                // THE FLOOR MUST NOT FOLLOW A LONE DIP. It used to adopt any lower
                // reading instantly — so a single ADC outlier (a steady ~1221 reporting
                // ONE sample of 4) collapsed the floor to 4, and the very next block
                // read 1221 >= base + fire_d and FIRED THE PAD at near-max velocity,
                // with ~1.7 s of base++ recovery. A false TRIGGER, not a click: the most
                // musically destructive thing the CV-spike audit turned up.
                // The dip must now persist for two blocks to be believed. Detection
                // latency is untouched (the arm/fire tests still read the raw pin), so
                // short gates still land.
                if (v < L->base) {
                    if (L->dip_seen) L->base = v;       // the dip is real: follow it
                    else L->dip_seen = true;            // first sighting: wait one block
                } else {
                    L->dip_seen = false;
                    if (L->base < 4095) L->base++;      // ~690/s upward drift back
                }
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
    int32_t fxbL[MACHINE_BLOCK / 2], fxbR[MACHINE_BLOCK / 2];   // FX1/FX2 bus
    int32_t sndL[MACHINE_BLOCK / 2], sndR[MACHINE_BLOCK / 2];   // reverb SEND bus
    memset(accL, 0, sizeof(accL));
    memset(accR, 0, sizeof(accR));
    memset(fxbL, 0, sizeof(fxbL));
    memset(fxbR, 0, sizeof(fxbR));
    memset(sndL, 0, sizeof(sndL));
    memset(sndR, 0, sizeof(sndR));
    // the rack runs whenever a generic slot holds an effect — even over a
    // silent bus, or a delay/flanger tail would be chopped mid-ring
    bool fx_live = dr.fx_slot[0] != FXK_OFF || dr.fx_slot[1] != FXK_OFF;
    bool any = false;
    for (int i = 0; i < DR_PADS; i++) {
        dr_pad_t *p = &dr.pad[i];
        if (!p->playing) continue;
        dr_voice_t v;
        voice_setup(p, &v);
        if (!v.buf || !v.len) { p->playing = false; continue; }
        any = true;
        // per-pad routing: wet pads mix into the FX bus (one shared chain),
        // dry pads straight into the master. The reverb send below is taken
        // from the pad either way — reverb stays its own send architecture.
        int32_t *mixL = (fx_live && p->fx_on) ? fxbL : accL;
        int32_t *mixR = (fx_live && p->fx_on) ? fxbR : accR;
        uint32_t len = v.len, df = v.df, af = v.af, st = v.st, ll = v.ll;
        uint32_t step = v.step;
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
                    step = v.step;
                    p->pos = st;
                    p->pos_fr = 0;
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
            // PITCH: the cursor is fractional (pos_fr under pos), so the read
            // linearly interpolates toward the next sample — inside the loop
            // window the neighbour wraps with it, at the buffer end it clamps.
            uint32_t lel = (pos > st) ? pos - st : 0;
            uint32_t idx = ll ? st + (lel % ll) : pos;
            uint32_t nxt = ll ? st + ((lel + 1) % ll)
                              : (idx + 1 < len ? idx + 1 : idx);
            int s0 = v.buf[idx];
            int s = s0 + (((v.buf[nxt] - s0) * (int)p->pos_fr) >> 12);
            s = (s * env) >> 8;
            // gain: 255 = unity, above that the pad is driven. Clip SOFTLY —
            // letting a 4x sample slam into the int16 clamp is fuzz, not drive.
            int g = ((int)p->level * (int)p->vel) >> 8;      // 0..DR_LEVEL_MAX
            int x = (s * g) >> 8;
            if (p->level > DR_LEVEL_UNITY) x = soft_clip(x);
            int32_t xl = (x * (255 - p->pan)) >> 8;
            int32_t xr = (x * p->pan) >> 8;
            mixL[f] += xl;
            mixR[f] += xr;
            if (p->rv_send) {                 // post-fader, post-pan send
                sndL[f] += (xl * p->rv_send) >> 8;
                sndR[f] += (xr * p->rv_send) >> 8;
            }
            uint32_t adv = p->pos_fr + step;  // fractional advance (Q12 pitch)
            p->pos = pos + (adv >> 12);
            p->pos_fr = adv & 0xFFF;
        }
    }
    // ---- master filter + output ------------------------------------------------
    // the filter, reverb AND rack must keep running after the last pad dies:
    // their rings/tails are still decaying
    bool flt_live = dr.flt_box && dr.flt_on;
    bool rv_live = (dr.rv.mode != RV_OFF) && dr.rv.slab;
    if (!any && !flt_live && !rv_live && !fx_live) return;

    // ---- FX bus through the rack's generic slots (FX1/FX2) ----------------------
    // pack the wet bus into a machine-format block, run the shared chain, fold
    // the result into the master sum ahead of the filter/reverb stages
    if (fx_live) {
        int32_t fxblk[MACHINE_BLOCK];
        for (int f = 0; f < frames; f++) {
            int32_t l = fxbL[f], r = fxbR[f];
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            fxblk[f * 2]     = l << 16;
            fxblk[f * 2 + 1] = r << 16;
        }
        fxrack_process_gen_i32(&dr_rk, fxblk, frames);
        for (int f = 0; f < frames; f++) {
            accL[f] += fxblk[f * 2] >> 16;
            accR[f] += fxblk[f * 2 + 1] >> 16;
        }
    }

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
        if (dr.rv_post && flt_live && dr.flt_mode) {
            // POST tap: re-derive the send from the FILTERED mix, keeping each
            // pad's send weighting. The filter is linear, so filtering the
            // pre-send bus is equivalent to scaling the filtered mix by the
            // bus/dry ratio — cheaper and phase-true (no second SVF pair).
            int32_t dl = accL[f], dr_ = accR[f];
            if (dl > 32767) dl = 32767; else if (dl < -32768) dl = -32768;
            if (dr_ > 32767) dr_ = 32767; else if (dr_ < -32768) dr_ = -32768;
            sndL[f] = dl ? (int32_t)(((int64_t)sndL[f] * lo32) / dl) : 0;
            sndR[f] = dr_ ? (int32_t)(((int64_t)sndR[f] * ro32) / dr_) : 0;
        }
    }
    if (rv_live) {
        // SEND bus: only what the pads sent goes into the tank; the dry mix
        // (already filtered) passes untouched and the return is added on top.
        // Send Tap (Setup) decides whether the bus was taken before the filter
        // (tail blooms through a closed sweep) or after it (tank ducks with
        // the kit) — the loop above rescales the bus for the POST case.
        int16_t send[MACHINE_BLOCK];
        for (int f = 0; f < frames; f++) {
            int32_t sl = sndL[f], sr = sndR[f];
            if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
            if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
            send[f * 2]     = (int16_t)sl;
            send[f * 2 + 1] = (int16_t)sr;
        }
        reverb_send_i32(&dr.rv, out, send, frames);
    }
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
    // the rack owns the FX serialization (slots + every effect param). It
    // writes the same "rv"/"rvmx" keys the old preset used, so reverb state
    // round-trips; a PRE-RACK preset's "dly" bool + params migrate through
    // fxrack_load's legacy path (dly:true -> FX1 = Delay).
    fxrack_save(&dr_rk, o);
    cJSON_AddBoolToObject(o, "rvpost", dr.rv_post);   // send tap pre/post filter
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
        cJSON_AddNumberToObject(p, "rvs", dr.pad[i].rv_send);
        cJSON_AddNumberToObject(p, "dec", dr.pad[i].decay_ms);
        cJSON_AddBoolToObject(p, "en", dr.pad[i].enabled);
        cJSON_AddNumberToObject(p, "src", dr.pad[i].ly[0].trig_src + 1);  // CV number, 1-based
        cJSON_AddBoolToObject(p, "lay", dr.pad[i].layered);               // B layer on?
        cJSON_AddStringToObject(p, "s2", dr.pad[i].ly[1].sample);         // the B layer
        cJSON_AddNumberToObject(p, "src2", dr.pad[i].ly[1].trig_src + 1); // (0 = none)
        cJSON_AddNumberToObject(p, "cw", dr.pad[i].cw_mode);         // knob7 clockwise target
        cJSON_AddNumberToObject(p, "reps", dr.pad[i].loop_reps);     // retrig cap (255 = INF)
        cJSON_AddBoolToObject(p, "fx", dr.pad[i].fx_on);             // FX1/FX2 bus routing
        cJSON_AddNumberToObject(p, "pit", dr.pad[i].pitch_semi);     // -12..+12 semitones
        cJSON_AddNumberToObject(p, "pcv", dr.pad[i].pitch_src + 1);  // pitch CV (0 = none)
        cJSON_AddNumberToObject(p, "pcm", dr.pad[i].pitch_mode);     // +/- vs V/oct
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
    // the rack owns the FX deserialization: slots + params, reverb mode/mix
    // (same "rv"/"rvmx" keys as before), and the legacy migration — an old
    // preset's "dly":true lands in FX1 = Delay with its params intact
    fxrack_load(&dr_rk, node);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rvpost"))) dr.rv_post = cJSON_IsTrue(j);
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
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "rvs")) && cJSON_IsNumber(j)) {
                int rs = j->valueint;               // clamp, don't mask (house lesson)
                dr.pad[i].rv_send = (uint8_t)(rs < 0 ? 0 : (rs > 255 ? 255 : rs));
            }
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
            // absent (pre-routing preset) leaves the default: wet
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "fx")) && cJSON_IsBool(j))
                dr.pad[i].fx_on = cJSON_IsTrue(j);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "pit")) && cJSON_IsNumber(j)) {
                int ps = j->valueint;
                if (ps < -12) ps = -12;
                if (ps > 12) ps = 12;
                dr.pad[i].pitch_semi = (int8_t)ps;
            }
            // pcv is 1-based; 0/absent = none (the src convention)
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "pcv")) && cJSON_IsNumber(j))
                dr.pad[i].pitch_src = (j->valueint <= 0) ? DR_SRC_NONE
                                                         : ((j->valueint - 1) & 7);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "pcm")) && cJSON_IsNumber(j))
                dr.pad[i].pitch_mode = (j->valueint == DR_PCV_VOCT) ? DR_PCV_VOCT
                                                                    : DR_PCV_BI;
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
