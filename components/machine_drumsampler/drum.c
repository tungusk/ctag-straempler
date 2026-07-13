// Drum sampler engine — CV-triggered one-shot pads mixed to stereo.
// All-RAM playback (SD only at load time), polyphonic across all pads.
#include <string.h>
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
static uint32_t pad_start(const dr_pad_t *p)
{
    if (!p->start_off || !p->len) return 0;
    uint32_t st = (uint32_t)(((uint64_t)p->start_off * p->len) >> 8);
    if (st >= p->len - 64) st = p->len - 64;
    return st;
}

static void trigger_pad(dr_pad_t *p, uint8_t vel)
{
    if (!p->enabled || p->len == 0) return;
    if (p->playing) {
        p->vel_next = vel;      // fade the running voice out, then restart
        p->fade = 256;
        p->retrig = true;
    } else {
        p->pos = pad_start(p);
        p->vel = vel;
        p->retrig = false;
        p->playing = true;
    }
    p->hit = true;
}

static esp_err_t drum_start(void)
{
    memset(&dr, 0, sizeof(dr));
    for (int i = 0; i < DR_PADS; i++) {
        dr.pad[i].buf = heap_caps_malloc((size_t)DR_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!dr.pad[i].buf) {
            ESP_LOGE("DRUM", "PSRAM alloc failed (pad %d)", i);
            for (int k = 0; k < i; k++) { free(dr.pad[k].buf); dr.pad[k].buf = NULL; }
            return ESP_ERR_NO_MEM;
        }
        dr.pad[i].enabled = true;
        dr.pad[i].level = 255;
        dr.pad[i].pan = 128;
        dr.pad[i].trig_src = i;
        dr.pad[i].base = 4095;     // floor tracker converges down on first reads
    }
    dr.n_pads = 4;          // 4 big pads is the default kit (Arlo); Setup goes to 8
    dr.cv_mod = true;       // knob6/knob7 perform the selected pad
    dr.sens = 1;            // Med
    dr.sel_src[0] = 5;      // knob6/knob7 — the two fully-good CV channels
    dr.sel_src[1] = 6;
    dr.prev_trig = 0x03;    // gates idle high; no phantom edge on boot
    audio_status_set_voices("drums", "");
    return ESP_OK;
}

static void drum_stop(void)
{
    for (int i = 0; i < DR_PADS; i++) { free(dr.pad[i].buf); dr.pad[i].buf = NULL; }
}

static void drum_process(int32_t out[MACHINE_BLOCK],
                         const int32_t in[MACHINE_BLOCK],
                         const machine_io_t *io)
{
    if (!dr.pad[0].buf) return;     // out is pre-zeroed by the core

    // ---- CV6/CV7 perform the selected pad (move-to-take-over; see drum_priv.h) --
    // A channel already spoken for as a CV-select selector is left alone — the
    // selectors default to exactly these two channels, and one knob can't both
    // address a pad and set its level. In Direct mode the selectors don't exist,
    // so nothing blocks the knobs there.
    if (dr.cv_mod) {
        static int last_lv = -1, last_dc = -1;
        bool lv_free = !(dr.cv_select && (dr.sel_src[0] == DR_MOD_LEVEL_CV ||
                                          dr.sel_src[1] == DR_MOD_LEVEL_CV));
        bool dc_free = !(dr.cv_select && (dr.sel_src[0] == DR_MOD_DECAY_CV ||
                                          dr.sel_src[1] == DR_MOD_DECAY_CV));
        int sel = dr.sel_pad;
        if (sel < 0 || sel >= dr.n_pads) sel = 0;
        dr_pad_t *sp = &dr.pad[sel];
        int lv = io->cv[DR_MOD_LEVEL_CV], dc = io->cv[DR_MOD_DECAY_CV];
        if (last_lv < 0) { last_lv = lv; last_dc = dc; }   // first block: adopt, don't apply
        int dlv = lv - last_lv, ddc = dc - last_dc;
        if (dlv < 0) dlv = -dlv;
        if (ddc < 0) ddc = -ddc;
        if (lv_free && dlv > DR_MOD_MOVE) {
            last_lv = lv;
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
            last_dc = dc;
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
                if (sp->cw_mode == DR_CW_START) {
                    sp->start_off = (uint8_t)(cw * DR_START_MAX / 2047);
                } else if (sp->cw_mode == DR_CW_ATTACK) {
                    sp->attack_ms = (uint16_t)(cw * DR_ATTACK_MAX / 2047);
                } else if (cw > 40) {                         // LOOP: dead spot at noon
                    // further clockwise = SHORTER loop (Arlo): a roll that tightens
                    // into a buzz as you turn
                    uint32_t span = DR_LOOP_MAX_MS - DR_LOOP_MIN_MS;
                    sp->loop_ms = (uint16_t)(DR_LOOP_MAX_MS - cw * span / 2047);
                }
            }
        }
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
        for (int ch = 0; ch < dr.n_pads; ch++) {
            dr_pad_t *p = &dr.pad[ch];
            int v = io->cv[p->trig_src & 7];
            if (v < p->base) p->base = v;               // dips pull the floor down instantly
            else if (p->base < 4095) p->base++;         // ~690/s upward drift back
            if (!p->armed) {
                if (v < p->base + arm_d[sens]) p->armed = true;
            } else if (v >= p->base + fire_d[sens]) {
                p->armed = false;
                int vel = (v - p->base) >> 3;           // velocity from swing above floor
                if (vel > 255) vel = 255;
                trigger_pad(p, dr.velocity ? (uint8_t)vel : 255);
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
            int idx = (int)((uint32_t)sv * (uint32_t)dr.n_pads / 4096);
            if (idx >= dr.n_pads) idx = dr.n_pads - 1;
            trigger_pad(&dr.pad[idx], 255);
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
        any = true;
        uint32_t len = p->len;
        uint32_t df = (uint32_t)p->decay_ms * 441 / 10;   // decay length in frames
        uint32_t af = (uint32_t)p->attack_ms * 441 / 10;  // attack length in frames
        uint32_t st = pad_start(p);                       // hits begin here
        // stutter loop: the read wraps inside [st, st+ll) while the pad's LIFE is
        // still the whole sample — so the roll ends when the one-shot would have,
        // instead of droning. A loop longer than what's left is no loop at all.
        uint32_t ll = (uint32_t)p->loop_ms * 441 / 10;
        if (ll >= len - st) ll = 0;
        for (int f = 0; f < frames; f++) {
            if (p->retrig) {
                // ~0.7 ms fade of the old voice, then restart at the new hit
                p->fade -= 8;
                if (p->fade <= 0) {
                    p->retrig = false;
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
            int s = (p->buf[idx] * env) >> 8;
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
    if (!any) return;
    for (int f = 0; f < frames; f++) {
        int32_t l = accL[f], r = accR[f];
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[f * 2]     = l << 16;
        out[f * 2 + 1] = r << 16;
    }
}

// ---- sample I/O (UI task) ---------------------------------------------------
// pad-cell thumbnail: peak per column straight out of the RAM buffer. No SD
// pass and no yield needed — 48 columns over a 2 s mono buffer at most.
static void drum_build_wf(dr_pad_t *p)
{
    p->wf_valid = false;
    if (!p->len) return;
    for (int c = 0; c < DR_WF_W; c++) {
        uint32_t a = (uint32_t)((uint64_t)c * p->len / DR_WF_W);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * p->len / DR_WF_W);
        if (b <= a) b = a + 1;
        if (b > p->len) b = p->len;
        int peak = 0;
        for (uint32_t k = a; k < b; k++) {
            int s = p->buf[k];
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        p->wf[c] = (uint8_t)(peak >> 7);      // 0..32767 -> 0..255, sampler scale
    }
    p->wf_valid = true;
}

int drum_load_pad(int pad, const char *name)
{
    if (pad < 0 || pad >= DR_PADS || !dr.pad[pad].buf) return -1;
    dr_pad_t *p = &dr.pad[pad];
    p->playing = false;
    p->retrig = false;
    p->len = 0;                      // engine stops reading before we overwrite
    p->wf_valid = false;
    uint32_t n = sample_load(name, p->buf, DR_MAX_FRAMES, true);   // mono
    if (n == 0) { p->sample[0] = 0; return -1; }
    strncpy(p->sample, name, sizeof(p->sample) - 1);
    p->sample[sizeof(p->sample) - 1] = 0;
    p->len = n;
    drum_build_wf(p);
    ESP_LOGI("DRUM", "pad %d: %s (%lu frames)", pad + 1, name, (unsigned long)n);
    return 0;
}

void drum_clear_pad(int pad)
{
    if (pad < 0 || pad >= DR_PADS) return;
    dr_pad_t *p = &dr.pad[pad];
    p->playing = false;
    p->retrig = false;
    p->len = 0;
    p->sample[0] = 0;
    p->wf_valid = false;
}

// ---- preset -----------------------------------------------------------------
static cJSON *drum_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "pads", dr.n_pads);
    cJSON_AddBoolToObject(o, "cvsel", dr.cv_select);
    cJSON_AddBoolToObject(o, "vel", dr.velocity);
    cJSON_AddBoolToObject(o, "cvmod", dr.cv_mod);
    cJSON_AddNumberToObject(o, "sens", dr.sens);
    cJSON_AddNumberToObject(o, "sel0", dr.sel_src[0]);
    cJSON_AddNumberToObject(o, "sel1", dr.sel_src[1]);
    cJSON *pads = cJSON_AddArrayToObject(o, "pad");
    for (int i = 0; i < DR_PADS; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "s", dr.pad[i].sample);
        cJSON_AddNumberToObject(p, "lvl", dr.pad[i].level);
        cJSON_AddNumberToObject(p, "pan", dr.pad[i].pan);
        cJSON_AddNumberToObject(p, "dec", dr.pad[i].decay_ms);
        cJSON_AddBoolToObject(p, "en", dr.pad[i].enabled);
        cJSON_AddNumberToObject(p, "src", dr.pad[i].trig_src + 1);   // CV number, 1-based
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
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "pads")) && cJSON_IsNumber(j))
        dr.n_pads = (j->valueint == 4) ? 4 : 8;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cvsel"))) dr.cv_select = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "vel")))   dr.velocity = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cvmod")))  dr.cv_mod = cJSON_IsTrue(j);
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
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "src")) && cJSON_IsNumber(j)) dr.pad[i].trig_src = (j->valueint - 1) & 7;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "cw")) && cJSON_IsNumber(j))
                dr.pad[i].cw_mode = (uint8_t)(j->valueint % DR_CW_MODES);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "reps")) && cJSON_IsNumber(j)) {
                int rp = j->valueint;
                if (rp < 0) rp = 0;
                if (rp > DR_REPS_INF) rp = DR_REPS_INF;
                dr.pad[i].loop_reps = (uint8_t)rp;
            }
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "s")) && cJSON_IsString(j) && j->valuestring[0])
                drum_load_pad(i, j->valuestring);   // reload the remembered sample
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
