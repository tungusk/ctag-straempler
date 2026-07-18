// Keys — tonal instrument sampler engine (see instsampler_priv.h). One mono
// PSRAM-resident sample, varispeed-pitched from CV1 (1V/oct) with a forward
// sustain loop, the Synth's linear ADSR + env-opened SVF low-pass, four macro
// knobs with takeover, and an 8-destination CV matrix. process() reads PSRAM
// only — no SD/heap/blocking (a loading gate keeps it silent during a swap).
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "cvsmooth.h"
#include "svf.h"
#include "sample_ram.h"
#include "preset_store.h"
#include "fxchain.h"
#include "instsampler_priv.h"

is_state_t inst;

// 4-point cubic (Catmull-Rom) interpolation over the mono int16 buffer at the
// fractional cursor pos. Tonal material holds pitched partials, so cubic (not
// the Slicer's linear) keeps the high end clean under varispeed. Returns an
// int16-scale float.
static inline float cubic_read(const int16_t *b, uint32_t n, double pos)
{
    long i1 = (long)pos;
    float t = (float)(pos - (double)i1);
    long i0 = i1 - 1, i2 = i1 + 1, i3 = i1 + 2;
    if (i0 < 0) i0 = 0;
    if (i1 >= (long)n) i1 = n - 1;
    if (i2 >= (long)n) i2 = n - 1;
    if (i3 >= (long)n) i3 = n - 1;
    float y0 = b[i0], y1 = b[i1], y2 = b[i2], y3 = b[i3];
    float a0 = -0.5f*y0 + 1.5f*y1 - 1.5f*y2 + 0.5f*y3;
    float a1 =       y0 - 2.5f*y1 + 2.0f*y2 - 0.5f*y3;
    float a2 = -0.5f*y0            + 0.5f*y2;
    return ((a0*t + a1)*t + a2)*t + y1;
}

// ch1/2 (1V/oct jacks) idle high (~21%), so rescale a matrix CV read from its
// tracked idle floor so a patched source spans the full 0..1 (as Synth/Sampler3).
static inline float is_mtx_cv01(const int *cvm, const int *floor, int src)
{
    int c = cvm[src & 7];
    if ((src & 7) < 2) {
        int fl = floor[src & 7];
        if (fl > 3800) return 0.0f;                     // channel not converged / dead
        c = (int)((int32_t)(c - fl) * 4095 / (4095 - fl));
        if (c < 0) c = 0;
        if (c > 4095) c = 4095;
    }
    return (float)c / 4095.0f;
}

int keys_load_zone(const char *name)
{
    is_zone_t *z = &inst.zone[0];
    if (!name || !name[0]) return -1;
    if (!z->buf) {
        z->buf = heap_caps_malloc((size_t)IS_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!z->buf) return -1;
    }
    inst.loading = true;
    uint32_t n = sample_load(name, z->buf, IS_MAX_FRAMES, true);   // mono, DMA-staged, sd_lock
    if (n < 2) { z->frames = 0; z->sample[0] = 0; inst.loading = false; return -1; }
    z->frames = n;
    strlcpy(z->sample, name, sizeof(z->sample));
    z->root = (uint8_t)inst.base_note;
    z->loop_start = 0;
    z->loop_end = n;
    if (z->loop_xfade == 0) z->loop_xfade = 220;         // ~5 ms default
    // waveform preview peaks (rectified max per column)
    for (int c = 0; c < IS_PEAKS; c++) {
        uint32_t a = (uint32_t)((uint64_t)c * n / IS_PEAKS);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * n / IS_PEAKS);
        uint32_t step = (b - a) / 48 + 1;
        int pk = 0;
        for (uint32_t s = a; s < b; s += step) { int v = z->buf[s]; if (v < 0) v = -v; if (v > pk) pk = v; }
        pk >>= 7;
        inst.peaks[c] = (uint8_t)(pk > 255 ? 255 : pk);
    }
    inst.loading = false;
    return 0;
}

uint32_t keys_snap_zero(uint32_t frame)
{
    is_zone_t *z = &inst.zone[0];
    if (!z->buf || z->frames < 2) return frame;
    if (frame >= z->frames) frame = z->frames - 1;
    for (int r = 0; r < 512; r++)                        // nearest rising zero-cross within +/-512
        for (int s = -1; s <= 1; s += 2) {
            long i = (long)frame + (long)s * r;
            if (i < 1 || i >= (long)z->frames) continue;
            if (z->buf[i - 1] <= 0 && z->buf[i] > 0) return (uint32_t)i;
        }
    return frame;
}

static esp_err_t keys_start(void)
{
    memset(&inst, 0, sizeof(inst));
    inst.base_note = 48;          // C3
    inst.quantize = true;
    inst.atk = 0.005f; inst.dec = 0.20f; inst.sus = 0.8f; inst.rel = 0.30f;
    inst.env_to_cut = 0.4f;
    inst.cutoff_base = 2000.0f;
    inst.res01 = 0.15f;
    inst.glide = 0.0f;
    inst.level = 0.85f;
    inst.start_frac = 0.0f;
    inst.knob_ctx = -1;           // force a knob recapture on the first block
    for (int d = 0; d < ISM_N; d++) { inst.mtx_src[d] = -1; inst.mtx_amt[d] = 0.0f; }
    inst.cv12_floor[0] = inst.cv12_floor[1] = 4095;
    inst.zone[0].root = 48;
    inst.zone[0].loop_mode = LOOP_FWD;
    inst.zone[0].loop_xfade = 220;
    svf_reset(&inst.voice[0].flt);
    fxfilter_init(&inst.filt);     // FX rack filter brick
    inst.fx_slot[0] = inst.fx_slot[1] = FXK_OFF;   // rack empty until assigned
    return ESP_OK;
}

static void keys_stop(void)
{
    if (inst.zone[0].buf) { heap_caps_free(inst.zone[0].buf); inst.zone[0].buf = NULL; }
    inst.zone[0].frames = 0;
    reverb_free(&inst.rv);
    fxdelay_free(&inst.dly);
    flanger_free(&inst.flg);
}

static void keys_process(int32_t out[MACHINE_BLOCK],
                         const int32_t in[MACHINE_BLOCK],
                         const machine_io_t *io)
{
    (void)in;
    is_zone_t  *z = &inst.zone[0];
    is_voice_t *v = &inst.voice[0];

    static cvmed_t med[8];
    int cvm[8];
    for (int k = 0; k < 8; k++) cvm[k] = cvmed_step(&med[k], io->cv[k]);

    // four macro knobs (ch5..8) with takeover: default holds until moved
    float kn[4] = { (float)cvm[4]/4095.0f, (float)cvm[5]/4095.0f,
                    (float)cvm[6]/4095.0f, (float)cvm[7]/4095.0f };
    if (inst.knob_ctx != 0) {
        inst.knob_ctx = 0;
        for (int i = 0; i < 4; i++) { inst.knob_capt[i] = kn[i]; inst.knob_live[i] = false; }
    }
    for (int i = 0; i < 4; i++)
        if (!inst.knob_live[i] && fabsf(kn[i] - inst.knob_capt[i]) > 0.03f) inst.knob_live[i] = true;
    if (inst.knob_live[0]) inst.start_frac  = kn[0];                       // K5 = start offset
    if (inst.knob_live[1]) inst.cutoff_base = 10.0f * powf(600.0f, kn[1]); // K6 = cutoff (log)
    if (inst.knob_live[2]) inst.res01       = kn[2];                       // K7 = resonance
    if (inst.knob_live[3]) inst.env_to_cut  = kn[3];                       // K8 = env>cut

    inst.cv1_disp = cvm[0];
    float note;
    if (audio_midi_gate()) {                          // web MIDI wins while held
        note = (float)audio_midi_note();
    } else {
        float semis = (float)(cvm[0] - IS_CV1_ZERO) / IS_CTS_PER_ST;
        note = (float)inst.base_note + semis;
    }
    if (inst.quantize) note = roundf(note);
    note = clampf(note, 0.0f, 127.0f);
    inst.note_disp = note;

    for (int c = 0; c < 2; c++) {                     // track ch1/2 idle floor
        int cv = io->cv[c];
        if (cv < inst.cv12_floor[c]) inst.cv12_floor[c] = cv;
        else if (inst.cv12_floor[c] < 4095) inst.cv12_floor[c]++;
    }

    // ---- CV matrix -> per-destination modulation on top of the base ----
    float m_cut = 0, m_res = 0, m_e2c = 0, m_lvl = 0, m_semi = 0, m_start = 0, m_lmov = 0, m_llen = 0;
    for (int d = 0; d < ISM_N; d++) {
        int src = inst.mtx_src[d];
        if (src < 0) continue;
        float cv01 = is_mtx_cv01(cvm, inst.cv12_floor, src);
        float a = inst.mtx_amt[d];
        switch (d) {
            case ISM_CUTOFF:  m_cut   += a * 4000.0f * cv01; break;
            case ISM_RES:     m_res   += a * cv01;           break;
            case ISM_ENVCUT:  m_e2c   += a * cv01;           break;
            case ISM_LEVEL:   m_lvl   += a * cv01;           break;
            case ISM_PITCH:   m_semi  += a * 24.0f * cv01;   break;
            case ISM_START:   m_start += a * cv01;           break;
            case ISM_LOOPMOV: m_lmov  += a * cv01;           break;
            case ISM_LOOPLEN: m_llen  += a * cv01;           break;
        }
    }
    float cutoff_eff = inst.cutoff_base + m_cut;
    if (cutoff_eff < 8.0f) cutoff_eff = 8.0f; else if (cutoff_eff > 6500.0f) cutoff_eff = 6500.0f;
    float res_eff = inst.res01 + m_res;      if (res_eff < 0) res_eff = 0; else if (res_eff > 1) res_eff = 1;
    float e2c_eff = inst.env_to_cut + m_e2c; if (e2c_eff < 0) e2c_eff = 0; else if (e2c_eff > 1) e2c_eff = 1;
    float lvl_eff = inst.level + m_lvl;      if (lvl_eff < 0) lvl_eff = 0; else if (lvl_eff > 1.2f) lvl_eff = 1.2f;

    // effective loop window (matrix LOOPMOV shifts it, LOOPLEN scales it)
    long ls = (long)z->loop_start, le = (long)z->loop_end;
    if (z->loop_mode != LOOP_OFF && z->frames > 0) {
        long mov = (long)(m_lmov * (float)z->frames);
        ls += mov; le += mov;
        long len = le - ls;
        len = (long)((float)len * (1.0f + m_llen));
        if (len < 64) len = 64;
        le = ls + len;
        if (ls < 0) ls = 0;
        if (le > (long)z->frames) le = (long)z->frames;
        if (le <= ls) le = ls + 64;
    }

    // gate on TR1 (active low; teleremote soft trigs already merged)
    bool g = !(io->trig_level & 1) || audio_midi_gate();
    if (g && !v->gate) {                              // note on (retrigger)
        v->env_stage = ENV_ATK;
        v->active = true;
        float sf = clampf(inst.start_frac + m_start, 0.0f, 0.99f);
        v->pos = (double)(sf * (float)z->frames);
    } else if (!g && v->gate) {
        if (v->env_stage != ENV_IDLE) v->env_stage = ENV_REL;
    }
    v->gate = g;

    // per-block envelope increments (linear; clamp times so we never div by 0)
    float atk = inst.atk > 0.0005f ? inst.atk : 0.0005f;
    float dec = inst.dec > 0.0005f ? inst.dec : 0.0005f;
    float rel = inst.rel > 0.0005f ? inst.rel : 0.0005f;
    float atk_inc = 1.0f / (atk * IS_RATE);
    float dec_inc = (1.0f - inst.sus) / (dec * IS_RATE);
    float rel_inc = 1.0f / (rel * IS_RATE);

    // glide: slew the sounding note toward the target
    if (v->cur_note <= 0.0f) v->cur_note = note;
    // freeze pitch while ungated: a released note's tail rings at its own pitch,
    // not the CV/base-note fallback (else every MIDI note-off blips to C3)
    if (!g) note = v->cur_note;
    if (inst.glide > 0.0005f) {
        float blockdur = (float)(MACHINE_BLOCK / 2) / IS_RATE;
        float coef = 1.0f - expf(-blockdur / inst.glide);
        v->cur_note += (note - v->cur_note) * coef;
    } else {
        v->cur_note = note;
    }
    float pnote = v->cur_note + m_semi;
    float inc = exp2f((pnote - (float)z->root) / 12.0f);
    if (inc < 0.25f) inc = 0.25f; else if (inc > 4.0f) inc = 4.0f;   // +/-2 octaves

    float q = svf_damp(res_eff, 0.6f, 2.0f);
    if (!(fabsf(v->flt.lp) < 1e9f) || !(fabsf(v->flt.bp) < 1e9f)) svf_reset(&v->flt);

    int frames = MACHINE_BLOCK / 2;
    bool silent = (z->frames == 0) || z->buf == NULL || inst.loading;
    for (int f = 0; f < frames; f++) {
        switch (v->env_stage) {
            case ENV_ATK: v->env += atk_inc; if (v->env >= 1.0f) { v->env = 1.0f; v->env_stage = ENV_DEC; } break;
            case ENV_DEC: v->env -= dec_inc; if (v->env <= inst.sus) { v->env = inst.sus; v->env_stage = ENV_SUS; } break;
            case ENV_SUS: v->env = inst.sus; break;
            case ENV_REL: v->env -= rel_inc; if (v->env <= 0.0f) { v->env = 0.0f; v->env_stage = ENV_IDLE; v->active = false; } break;
            default:      v->env = 0.0f; break;
        }

        float sig = 0.0f;
        if (!silent && v->active && v->env_stage != ENV_IDLE && v->pos < (double)z->frames) {
            sig = cubic_read(z->buf, z->frames, v->pos) / 32768.0f;
            // wrap crossfade: as the cursor nears loop_end, blend with the
            // material one loop-length back (continuous through the seam)
            if (z->loop_mode == LOOP_FWD && z->loop_xfade > 0 &&
                v->pos < (double)le && (double)le - v->pos < (double)z->loop_xfade) {
                double pos2 = v->pos - (double)(le - ls);
                if (pos2 >= 1.0) {
                    float fr = (float)(((double)z->loop_xfade - ((double)le - v->pos)) / (double)z->loop_xfade);
                    if (fr < 0) fr = 0; else if (fr > 1) fr = 1;
                    float sig2 = cubic_read(z->buf, z->frames, pos2) / 32768.0f;
                    sig = sig * cosf(fr * 1.5707963f) + sig2 * sinf(fr * 1.5707963f);
                }
            }
            v->pos += inc;
            if (z->loop_mode == LOOP_FWD) {
                if (v->pos >= (double)le) v->pos -= (double)(le - ls);
            } else if (v->pos >= (double)z->frames) {   // one-shot ended
                v->env_stage = ENV_IDLE; v->env = 0.0f; v->active = false;
            }

            float fc = cutoff_eff + v->env * e2c_eff * 5000.0f;
            if (fc < 8.0f) fc = 8.0f;
            float coef = svf_coef(fc, IS_RATE, 1.0f);
            float lp;
            svf_step(&v->flt, sig, coef, q, &lp, NULL, NULL);
            sig = lp * v->env;
        }

        float y = sig * lvl_eff * 28000.0f;
        if (y > 32000.0f) y = 32000.0f; else if (y < -32000.0f) y = -32000.0f;
        int32_t s = ((int32_t)(int16_t)y) << 16;
        out[f * 2] = s;
        out[f * 2 + 1] = s;
    }

    // FX chain: overdrive -> flanger -> tremolo -> delay -> reverb. Run in float
    // with a single soft limiter at the end (fxchain.h) so stacked effects don't
    // hard-clip at every stage — the old all-_block_i32 chain had no headroom.
    {
        float fb[FX_SCRATCH_N];
        fx_unpack_i32(out, fb, frames * 2);
        // FX rack: run the two generic slots in order (FX1 then FX2), then the
        // fixed reverb slot (FX3). Slot assignment = which effect + what order.
        for (int s = 0; s < FX_NSLOT_GEN; s++) {
            switch (inst.fx_slot[s]) {
                case FXK_OD:   overdrive_block_f(&inst.od, fb, frames); break;
                case FXK_FLG:  if (inst.flg.bufL) flanger_block_f(&inst.flg, fb, frames); break;
                case FXK_TREM: tremolo_block_f(&inst.trem, fb, frames); break;
                case FXK_DLY:  if (inst.dly.bufL) fxdelay_block_f(&inst.dly, fb, frames); break;
                case FXK_FILT: fxfilter_block_f(&inst.filt, fb, frames); break;
                default: break;
            }
        }
        if (inst.rv.mode != RV_OFF && inst.rv.slab)  reverb_block_f(&inst.rv, fb, frames);
        fx_pack_softclip(fb, out, frames * 2);
    }
}

// ---- preset (autosave + named recall reuse the same (de)serializers) --------
static cJSON *keys_preset_save(void)
{
    is_zone_t *z = &inst.zone[0];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "base", inst.base_note);
    cJSON_AddBoolToObject(o, "quant", inst.quantize);
    cJSON_AddNumberToObject(o, "atk", inst.atk);
    cJSON_AddNumberToObject(o, "dec", inst.dec);
    cJSON_AddNumberToObject(o, "sus", inst.sus);
    cJSON_AddNumberToObject(o, "rel", inst.rel);
    cJSON_AddNumberToObject(o, "e2c", inst.env_to_cut);
    cJSON_AddNumberToObject(o, "cut", inst.cutoff_base);
    cJSON_AddNumberToObject(o, "res", inst.res01);
    cJSON_AddNumberToObject(o, "gld", inst.glide);
    cJSON_AddNumberToObject(o, "lvl", inst.level);
    cJSON_AddNumberToObject(o, "rv", inst.rv.mode);
    cJSON_AddNumberToObject(o, "rvmx", (int)(inst.rv.wet * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "dly", inst.dly_on);
    cJSON_AddNumberToObject(o, "dlyt", (int)(fxdelay_time_ms(&inst.dly) + 0.5f));
    cJSON_AddNumberToObject(o, "dlyfb", (int)(inst.dly.fb * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "dlymx", (int)(inst.dly.wet * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "dlytn", (int)(inst.dly.damp * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "dlypp", inst.dly.pingpong);
    cJSON_AddBoolToObject(o, "od", inst.od_on);
    cJSON_AddNumberToObject(o, "oddr", (int)(inst.od.drive * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "odtn", (int)(inst.od.tone * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "odbs", (int)(inst.od.bias * 100 + (inst.od.bias < 0 ? -0.5f : 0.5f)));
    cJSON_AddNumberToObject(o, "odlv", (int)(inst.od.level * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "flg", inst.flg_on);
    cJSON_AddNumberToObject(o, "flgrt", (int)(inst.flg.rate * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "flgdp", (int)(inst.flg.depth * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "flgfb", (int)(inst.flg.fb * 100 + (inst.flg.fb < 0 ? -0.5f : 0.5f)));
    cJSON_AddNumberToObject(o, "flgmx", (int)(inst.flg.wet * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "trem", inst.trem_on);
    cJSON_AddNumberToObject(o, "trmrt", (int)(inst.trem.rate * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "trmdp", (int)(inst.trem.depth * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "trmsh", inst.trem.shape);
    cJSON_AddBoolToObject(o, "trmst", inst.trem.stereo);
    // FX rack: slot assignment (FX1,FX2) + filter brick params
    cJSON *sl = cJSON_AddArrayToObject(o, "fxsl");
    for (int s = 0; s < FX_NSLOT_GEN; s++) cJSON_AddItemToArray(sl, cJSON_CreateNumber(inst.fx_slot[s]));
    cJSON_AddNumberToObject(o, "fim", inst.filt.mode);
    cJSON_AddNumberToObject(o, "fic", (int)(inst.filt.cutoff * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "fiq", (int)(inst.filt.reso * 100 + 0.5f));
    cJSON_AddStringToObject(o, "smp", z->sample);
    cJSON_AddNumberToObject(o, "root", z->root);
    cJSON_AddNumberToObject(o, "lm", z->loop_mode);
    cJSON_AddNumberToObject(o, "ls", (double)z->loop_start);
    cJSON_AddNumberToObject(o, "le", (double)z->loop_end);
    cJSON_AddNumberToObject(o, "lx", (double)z->loop_xfade);
    cJSON *ms = cJSON_AddArrayToObject(o, "msrc");
    cJSON *ma = cJSON_AddArrayToObject(o, "mamt");
    for (int d = 0; d < ISM_N; d++) {
        cJSON_AddItemToArray(ms, cJSON_CreateNumber(inst.mtx_src[d]));
        cJSON_AddItemToArray(ma, cJSON_CreateNumber(inst.mtx_amt[d]));
    }
    return o;
}

static void keys_preset_load(const cJSON *node)
{
    if (!node) return;
    is_zone_t *z = &inst.zone[0];
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "base"))  && cJSON_IsNumber(j)) inst.base_note = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "quant")) && cJSON_IsBool(j))   inst.quantize = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "atk"))   && cJSON_IsNumber(j)) inst.atk = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "dec"))   && cJSON_IsNumber(j)) inst.dec = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sus"))   && cJSON_IsNumber(j)) inst.sus = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rel"))   && cJSON_IsNumber(j)) inst.rel = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "e2c"))   && cJSON_IsNumber(j)) inst.env_to_cut = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cut"))   && cJSON_IsNumber(j)) inst.cutoff_base = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "res"))   && cJSON_IsNumber(j)) inst.res01 = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "gld"))   && cJSON_IsNumber(j)) inst.glide = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lvl"))   && cJSON_IsNumber(j)) inst.level = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rv"))    && cJSON_IsNumber(j)) {
        int m = j->valueint; if (m < 0 || m >= RV_N_MODES) m = RV_OFF;
        if (m != RV_OFF && !inst.rv.slab && reverb_init(&inst.rv) != ESP_OK) m = RV_OFF;
        reverb_set_mode(&inst.rv, m);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rvmx"))  && cJSON_IsNumber(j)) reverb_set_mix(&inst.rv, (float)j->valueint / 100.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "dly"))   && cJSON_IsBool(j)) {
        bool on = cJSON_IsTrue(j);
        if (on && !inst.dly.bufL && fxdelay_init(&inst.dly) != ESP_OK) on = false;
        inst.dly_on = on;
    }
    if (inst.dly.bufL) {   // params apply once the slab exists (order-independent)
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlyt"))  && cJSON_IsNumber(j)) fxdelay_set_time_ms(&inst.dly, (float)j->valueint);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlyfb")) && cJSON_IsNumber(j)) fxdelay_set_feedback(&inst.dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlymx")) && cJSON_IsNumber(j)) fxdelay_set_mix(&inst.dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlytn")) && cJSON_IsNumber(j)) fxdelay_set_damp(&inst.dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlypp")) && cJSON_IsBool(j))   fxdelay_set_pingpong(&inst.dly, cJSON_IsTrue(j));
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "od"))    && cJSON_IsBool(j)) inst.od_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "oddr"))  && cJSON_IsNumber(j)) inst.od.drive = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odtn"))  && cJSON_IsNumber(j)) inst.od.tone = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odbs"))  && cJSON_IsNumber(j)) inst.od.bias = clampf((float)j->valueint / 100.0f, -1.0f, 1.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odlv"))  && cJSON_IsNumber(j)) inst.od.level = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "flg"))   && cJSON_IsBool(j)) {
        bool on = cJSON_IsTrue(j);
        if (on && !inst.flg.bufL && flanger_init(&inst.flg) != ESP_OK) on = false;
        inst.flg_on = on;
    }
    if (inst.flg.bufL) {   // params apply once the slab exists (order-independent)
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgrt")) && cJSON_IsNumber(j)) inst.flg.rate = clampf((float)j->valueint / 100.0f, 0.01f, 10.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgdp")) && cJSON_IsNumber(j)) inst.flg.depth = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgfb")) && cJSON_IsNumber(j)) inst.flg.fb = clampf((float)j->valueint / 100.0f, -0.95f, 0.95f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgmx")) && cJSON_IsNumber(j)) inst.flg.wet = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trem"))  && cJSON_IsBool(j)) inst.trem_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmrt")) && cJSON_IsNumber(j)) inst.trem.rate = clampf((float)j->valueint / 100.0f, 0.05f, 20.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmdp")) && cJSON_IsNumber(j)) inst.trem.depth = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmsh")) && cJSON_IsNumber(j)) { int s = j->valueint; inst.trem.shape = (s < 0 || s >= TREM_NSHAPE) ? TREM_SINE : s; }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmst")) && cJSON_IsBool(j)) inst.trem.stereo = cJSON_IsTrue(j);
    // filter brick params
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fim")) && cJSON_IsNumber(j)) { int m = j->valueint; inst.filt.mode = (m < 0 || m >= FILT_NMODE) ? FILT_LP : m; }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fic")) && cJSON_IsNumber(j)) inst.filt.cutoff = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fiq")) && cJSON_IsNumber(j)) inst.filt.reso = clampf((float)j->valueint / 100.0f, 0.0f, 1.0f);
    // FX slot assignment; migrate from the legacy *_on bools when absent
    cJSON *fsl = cJSON_GetObjectItemCaseSensitive(node, "fxsl");
    if (cJSON_IsArray(fsl)) {
        for (int s = 0; s < FX_NSLOT_GEN; s++) {
            cJSON *si = cJSON_GetArrayItem(fsl, s);
            int v = cJSON_IsNumber(si) ? si->valueint : FXK_OFF;
            inst.fx_slot[s] = (v < 0 || v >= FXK_NGEN) ? FXK_OFF : (int8_t)v;
        }
    } else {
        int s = 0;   // legacy: rebuild slots from the loaded enables (od,flg,trem,dly order)
        if (inst.od_on   && s < FX_NSLOT_GEN) inst.fx_slot[s++] = FXK_OD;
        if (inst.flg_on  && s < FX_NSLOT_GEN) inst.fx_slot[s++] = FXK_FLG;
        if (inst.trem_on && s < FX_NSLOT_GEN) inst.fx_slot[s++] = FXK_TREM;
        if (inst.dly_on  && s < FX_NSLOT_GEN) inst.fx_slot[s++] = FXK_DLY;
        while (s < FX_NSLOT_GEN) inst.fx_slot[s++] = FXK_OFF;
    }
    // load the sample FIRST (it resets loop points), then restore them
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "smp"))   && cJSON_IsString(j) && j->valuestring[0]) keys_load_zone(j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "root"))  && cJSON_IsNumber(j)) z->root = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lm"))    && cJSON_IsNumber(j)) z->loop_mode = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ls"))    && cJSON_IsNumber(j)) z->loop_start = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "le"))    && cJSON_IsNumber(j)) z->loop_end = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lx"))    && cJSON_IsNumber(j)) z->loop_xfade = (uint32_t)j->valuedouble;
    if (z->frames) {
        if (z->loop_end > z->frames) z->loop_end = z->frames;
        if (z->loop_start >= z->loop_end) z->loop_start = 0;
    }
    cJSON *ms = cJSON_GetObjectItemCaseSensitive(node, "msrc");
    cJSON *ma = cJSON_GetObjectItemCaseSensitive(node, "mamt");
    if (cJSON_IsArray(ms) && cJSON_IsArray(ma)) {
        for (int d = 0; d < ISM_N; d++) {
            cJSON *si = cJSON_GetArrayItem(ms, d), *ai = cJSON_GetArrayItem(ma, d);
            if (cJSON_IsNumber(si)) { int val = si->valueint; inst.mtx_src[d] = (val < -1 || val > 7) ? -1 : (int8_t)val; }
            if (cJSON_IsNumber(ai)) { float a = (float)ai->valuedouble; inst.mtx_amt[d] = a < -1 ? -1 : a > 1 ? 1 : a; }
        }
    }
    inst.knob_ctx = -1;   // re-arm knob takeover against the loaded values
}

// ---- named patch files: usr/keys/PAT_NNN.jsn (shared preset_store) ---------
// The autosave (de)serializers above do the state work; keys_preset_load
// already re-arms knob takeover (knob_ctx = -1) and reloads the sample.
static const preset_store_t KS_PS = { "/sdcard/usr/keys", "PAT_" };

int keys_patch_save(char *id_out, size_t n)
{
    return preset_store_save(&KS_PS, keys_preset_save(), id_out, n);
}

int keys_patch_load(const char *id)
{
    cJSON *root = preset_store_load(&KS_PS, id);
    if (!root) return -1;
    keys_preset_load(root);
    cJSON_Delete(root);
    return 0;
}

int keys_patch_list(char ids[][12], int max)
{
    return preset_store_list(&KS_PS, ids, max);
}

extern const machine_ui_t keys_menu_ui;

const machine_t machine_instsampler = {
    .name = "Keys",
    .start = keys_start,
    .stop = keys_stop,
    .process = keys_process,
    .preset_save = keys_preset_save,
    .preset_load = keys_preset_load,
    .ui = &keys_menu_ui,
};
