// Keys — tonal instrument sampler engine (see instsampler_priv.h). One mono
// PSRAM-resident sample, varispeed-pitched from CV1 (1V/oct) with a forward
// sustain loop, the Synth's linear ADSR + env-opened SVF low-pass, four macro
// knobs with takeover, and an 8-destination CV matrix. process() reads PSRAM
// only — no SD/heap/blocking (a loading gate keeps it silent during a swap).
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "machine.h"
#include "audio.h"
#include "cvsmooth.h"
#include "svf.h"
#include "sample_ram.h"
#include "pitch_detect.h"
#include "preset_store.h"
#include "fxchain.h"
#include "instsampler_priv.h"

is_state_t inst;
fxrack_t inst_rk;    // FX rack pointer-view (initialized in keys_start)

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

// CV matrix destination names (order = the ISM_* enum). Source conditioning +
// page + persistence live in the shared cvmtx widget now.
const char *const keys_mtx_labels[ISM_N] = {
    "Cutoff", "Reso", "Env>Cut", "Level", "Pitch", "Start", "LoopMov", "LoopLen"
};

// PSRAM is a 4 MB HARDWARE ceiling shared with the FX slabs (delay ~690 KB,
// flanger ~90 KB, reverb tank ~170 KB) and with Tape, which wants up to 3.62 MB —
// so this buffer cannot just be reserved for the session, and keys_stop frees it.
// But asking for ONE 2 MB block after that hole has been reused is a coin flip:
// measured 2026-07-26, the largest free block sat at 1,998,848 bytes — **1,152
// short** — and every load silently returned -1 for the rest of the session
// (Arlo: "i can't seem to load a sample into keys"; only a power cycle fixed it).
// So walk a LADDER down. A shorter maximum sample is a far better failure than a
// machine that cannot load anything, and the achieved size is remembered in
// z->cap so sample_load never overruns what we actually got.
static const uint32_t is_cap_ladder[] = { IS_MAX_FRAMES, 750000u, 500000u, 250000u, 120000u };

int keys_load_zone(const char *name)
{
    is_zone_t *z = &inst.zone[0];
    if (!name || !name[0]) return -1;
    inst.load_err[0] = 0;
    if (!z->buf) {
        for (int i = 0; i < (int)(sizeof is_cap_ladder / sizeof is_cap_ladder[0]); i++) {
            z->buf = heap_caps_malloc((size_t)is_cap_ladder[i] * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (z->buf) { z->cap = is_cap_ladder[i]; break; }
        }
        if (!z->buf) {
            z->cap = 0;
            snprintf(inst.load_err, sizeof(inst.load_err), "no PSRAM");
            return -1;
        }
        if (z->cap < IS_MAX_FRAMES)
            ESP_LOGW("keys", "sample buffer fell back to %u frames (%.1f s) — PSRAM fragmented",
                     (unsigned)z->cap, (double)z->cap / IS_RATE);
    }
    inst.loading = true;
    uint32_t n = sample_load(name, z->buf, z->cap, true);   // mono, DMA-staged, sd_lock
    if (n < 2) {
        z->frames = 0; z->sample[0] = 0; inst.loading = false;
        snprintf(inst.load_err, sizeof(inst.load_err), "load failed");
        return -1;
    }
    z->frames = n;
    strlcpy(z->sample, name, sizeof(z->sample));
    z->root = (uint8_t)inst.base_note;
    z->fine = 0.0f;
    inst.tune_hz = 0.0f;          // a new sample invalidates the last verdict
    inst.tune_conf = 0.0f;
    inst.tune_src = TUNE_NONE;
    inst.tune_hint = -1;
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
    if (inst.autotune_load) keys_autotune();     // sets root+fine, or leaves them
    inst.loading = false;
    return 0;
}

// Auto-tune: detect the loaded sample's fundamental and write it into the
// zone's root + fine so the sample plays IN TUNE across the keyboard (root is
// the whole semitone, fine the cents remainder — root alone can only ever get
// within half a semitone). Buffer is already PSRAM-resident, so this is pure
// DSP: no SD, no lock, ~50 ms. UI/loader context only, never the audio task.
// A low-confidence verdict (chord, noise hit, percussion) is REPORTED and
// DISCARDED rather than applied — a bad auto-tune is worse than none.
int keys_autotune(void)
{
    is_zone_t *z = &inst.zone[0];
    inst.tune_hz = 0.0f;
    inst.tune_conf = 0.0f;
    inst.tune_src = TUNE_NONE;
    if (!z->buf || z->frames < 4096) return -1;

    // the id often names the note it was recorded at ("EP_C4", "PNOF#3")
    bool hint_iso = false;
    int  hint = pitch_name_hint(z->sample, &hint_iso);
    inst.tune_hint = (int16_t)hint;

    pitch_result_t r = { 0 };     // a failed detect leaves it untouched
    bool heard = (pitch_detect_buf(z->buf, z->frames, (float)IS_RATE, &r) == 0) &&
                 r.conf >= 0.30f;
    if (heard) { inst.tune_hz = r.hz; inst.tune_conf = r.conf; }
    else if (r.hz > 0.0f) { inst.tune_hz = r.hz; inst.tune_conf = r.conf; }

    int   midi  = 0;
    float cents = 0.0f;
    if (heard) {
        midi  = r.midi;
        cents = r.cents;
        inst.tune_src = TUNE_AUDIO;
        // The audio is the authority on PITCH CLASS and cents; where detection
        // actually goes wrong is the OCTAVE (a chord or a strong sub-harmonic
        // reads an octave or two down). So a same-pitch-class name hint gets to
        // move the register, and nothing else. A hint that disagrees on pitch
        // class is reported, not obeyed — the recording beats the label.
        if (hint >= 0 && ((hint % 12) == (midi % 12))) {
            if (hint != midi) { midi = hint; inst.tune_src = TUNE_BOTH; }
        } else if (hint >= 0) {
            inst.tune_src = TUNE_CONFLICT;
        }
    } else if (hint >= 0 && hint_iso) {
        // nothing pitched heard (percussive/noisy) but the id names a note
        // outright — trust the label, and say so in the UI
        midi = hint;
        inst.tune_src = TUNE_NAME;
    } else {
        return -1;
    }

    z->root = (uint8_t)clampi(midi, 12, 108);
    z->fine = clampf(cents / 100.0f, -1.0f, 1.0f);
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
    cvmtx_init(&inst.mtx, keys_mtx_labels, ISM_N);   // matrix off, floors armed
    inst.zone[0].root = 48;
    inst.zone[0].loop_mode = LOOP_FWD;
    inst.zone[0].loop_xfade = 220;
    svf_reset(&inst.voice[0].flt);
    fxfilter_init(&inst.filt);     // FX rack filter bricks
    fxfilter_init(&inst.band);
    inst.fx_slot[0] = inst.fx_slot[1] = FXK_OFF;   // rack empty until assigned
    inst_rk = (fxrack_t){ .od = &inst.od, .flg = &inst.flg, .trem = &inst.trem, .dly = &inst.dly,
                          .filt = &inst.filt, .band = &inst.band, .rv = &inst.rv, .slot = inst.fx_slot };
    return ESP_OK;
}

static void keys_stop(void)
{
    // The buffer IS released here — Tape wants up to 3.62 MB of the 4 MB pool, so
    // holding 2 MB for a machine that is not running would break long takes. The
    // cost is that getting it back depends on PSRAM not having fragmented in the
    // meantime, which is why keys_load_zone walks a ladder instead of demanding
    // the full size.
    if (inst.zone[0].buf) { heap_caps_free(inst.zone[0].buf); inst.zone[0].buf = NULL; }
    inst.zone[0].cap = 0;
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
    // knob edits never reach the UI event queue: flag committed moves so the
    // autosave picks them up (hysteresis — live knobs track every block)
    {
        static float s_kdirty[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; i++)
            if (inst.knob_live[i] &&
                (s_kdirty[i] < 0 || fabsf(kn[i] - s_kdirty[i]) > 0.03f)) {
                s_kdirty[i] = kn[i];
                machine_state_dirty();
            }
    }

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

    cvmtx_track(&inst.mtx, cvm);                      // ch1/2 idle-floor follow

    // ---- CV matrix -> per-destination modulation on top of the base ----
    float m_cut = 0, m_res = 0, m_e2c = 0, m_lvl = 0, m_semi = 0, m_start = 0, m_lmov = 0, m_llen = 0;
    for (int d = 0; d < ISM_N; d++) {
        float av = cvmtx_val(&inst.mtx, cvm, d);      // amt * conditioned CV
        if (av == 0.0f) continue;
        switch (d) {
            case ISM_CUTOFF:  m_cut   += av * 4000.0f; break;
            case ISM_RES:     m_res   += av;           break;
            case ISM_ENVCUT:  m_e2c   += av;           break;
            case ISM_LEVEL:   m_lvl   += av;           break;
            case ISM_PITCH:   m_semi  += av * 24.0f;   break;
            case ISM_START:   m_start += av;           break;
            case ISM_LOOPMOV: m_lmov  += av;           break;
            case ISM_LOOPLEN: m_llen  += av;           break;
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
    float inc = exp2f((pnote - ((float)z->root + z->fine)) / 12.0f);
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

    // FX rack: generic slots in order -> reverb, float chain + soft limiter.
    fxrack_process_i32(&inst_rk, out, frames);
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
    fxrack_save(&inst_rk, o);   // slots + every effect param (shared FX rack)
    cJSON_AddStringToObject(o, "smp", z->sample);
    cJSON_AddNumberToObject(o, "root", z->root);
    cJSON_AddNumberToObject(o, "fn", z->fine);      // cents-as-semitones (auto-tune)
    cJSON_AddBoolToObject(o, "atl", inst.autotune_load);
    cJSON_AddNumberToObject(o, "lm", z->loop_mode);
    cJSON_AddNumberToObject(o, "ls", (double)z->loop_start);
    cJSON_AddNumberToObject(o, "le", (double)z->loop_end);
    cJSON_AddNumberToObject(o, "lx", (double)z->loop_xfade);
    // K5 start offset was knob-only (never persisted) until the autosave
    // sweep made knob edits savable
    cJSON_AddNumberToObject(o, "stf", inst.start_frac);
    cvmtx_save(&inst.mtx, o);                // matrix ("mxs"/"mxa")
    return o;
}

static void keys_preset_load(const cJSON *node)
{
    if (!node) return;
    is_zone_t *z = &inst.zone[0];
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "base"))  && cJSON_IsNumber(j)) inst.base_note = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "quant")) && cJSON_IsBool(j))   inst.quantize = cJSON_IsTrue(j);
    // read BEFORE "smp": it decides whether the reload below auto-tunes at all
    // (the stored root/fine then land on top either way)
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "atl"))   && cJSON_IsBool(j))   inst.autotune_load = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "atk"))   && cJSON_IsNumber(j)) inst.atk = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "dec"))   && cJSON_IsNumber(j)) inst.dec = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sus"))   && cJSON_IsNumber(j)) inst.sus = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rel"))   && cJSON_IsNumber(j)) inst.rel = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "e2c"))   && cJSON_IsNumber(j)) inst.env_to_cut = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cut"))   && cJSON_IsNumber(j)) inst.cutoff_base = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "res"))   && cJSON_IsNumber(j)) inst.res01 = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "gld"))   && cJSON_IsNumber(j)) inst.glide = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lvl"))   && cJSON_IsNumber(j)) inst.level = (float)j->valuedouble;
    fxrack_load(&inst_rk, node);   // slots + every effect param (shared FX rack)
    // load the sample FIRST (it resets loop points), then restore them
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "smp"))   && cJSON_IsString(j) && j->valuestring[0]) keys_load_zone(j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "root"))  && cJSON_IsNumber(j)) z->root = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fn"))    && cJSON_IsNumber(j)) z->fine = clampf((float)j->valuedouble, -1.0f, 1.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lm"))    && cJSON_IsNumber(j)) z->loop_mode = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "ls"))    && cJSON_IsNumber(j)) z->loop_start = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "le"))    && cJSON_IsNumber(j)) z->loop_end = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lx"))    && cJSON_IsNumber(j)) z->loop_xfade = (uint32_t)j->valuedouble;
    if (z->frames) {
        if (z->loop_end > z->frames) z->loop_end = z->frames;
        if (z->loop_start >= z->loop_end) z->loop_start = 0;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "stf")) && cJSON_IsNumber(j)) {
        float s = (float)j->valuedouble;
        inst.start_frac = s < 0 ? 0 : s > 0.99f ? 0.99f : s;
    }
    cvmtx_load(&inst.mtx, node);   // "mxs"/"mxa", with the legacy "msrc"/"mamt" fallback
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
