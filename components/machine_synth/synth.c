// Synth voice engine (see synth_priv.h). Monophonic subtractive voice:
// polyBLEP saw<->square osc -> SVF low-pass (env-modulated cutoff) -> ADSR VCA.
// Pitch on CV1 (1V/oct), gate on TR1. process() is pure DSP — no SD/heap/blocking.
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
#include "synth_priv.h"

sy_state_t sy;
fxrack_t sy_rk;    // FX rack pointer-view (initialized in synth_start)

// anti-aliasing correction at a phase discontinuity (t = phase, dt = phase inc)
static inline float polyblep(float t, float dt)
{
    if (t < dt)            { t /= dt;          return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt)     { t = (t - 1.0f)/dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static void note_from_cv(int cv1)
{
    float semis = (float)(cv1 - SY_CV1_ZERO) / SY_CTS_PER_ST;   // offset from base note
    float note = (float)sy.base_note + semis;
    if (sy.quantize) note = roundf(note);
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    sy.freq = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);      // MIDI note -> Hz
}

int synth_load_wave(const char *name)
{
    if (!name || !name[0]) return -1;
    if (!sy.wave) {
        sy.wave = heap_caps_malloc((size_t)SY_WT_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!sy.wave) return -1;
    }
    uint32_t n = sample_load(name, sy.wave, SY_WT_MAX, true);   // mono
    if (n < 2) { sy.wave_len = 0; sy.wave_name[0] = 0; return -1; }
    sy.wave_len = (int)n;
    strlcpy(sy.wave_name, name, sizeof(sy.wave_name));
    return 0;
}

static esp_err_t synth_start(void)
{
    memset(&sy, 0, sizeof(sy));
    sy.engine = ENG_VA;
    sy.base_note = 48;          // C3
    sy.quantize = true;
    sy.shape = 0.15f;           // mostly saw
    sy.fm_ratio = 2.0f;         // FM: modulator = 2x carrier
    sy.fm_index = 3.0f;         // FM: a bright-ish plucky depth
    sy.atk = 0.005f; sy.dec = 0.20f; sy.sus = 0.7f; sy.rel = 0.30f;
    sy.env_to_cut = 0.5f;
    sy.glide = 0.0f;
    sy.cur_freq = 0.0f;
    sy.lfo_rate = 5.0f;
    sy.lfo_depth = 0.0f;
    sy.lfo_dest = LFO_OFF;
    sy.level = 0.8f;
    sy.cutoff_base = 1200.0f;
    sy.res01 = 0.2f;
    sy.freq = 261.6f;           // C4-ish until CV read
    sy.knob_engine = -1;        // force a knob recapture on the first block
    cvmtx_init(&sy.mtx, synth_mtx_labels, SYM_N);      // matrix off, floors armed
    svf_reset(&sy.flt_l);
    fxfilter_init(&sy.filt);
    fxfilter_init(&sy.band);
    sy.fx_slot[0] = sy.fx_slot[1] = FXK_OFF;
    sy_rk = (fxrack_t){ .od = &sy.od, .flg = &sy.flg, .trem = &sy.trem, .dly = &sy.dly,
                        .filt = &sy.filt, .band = &sy.band, .rv = &sy.rv, .slot = sy.fx_slot };
    return ESP_OK;
}

static void synth_stop(void)
{
    if (sy.wave) { heap_caps_free(sy.wave); sy.wave = NULL; }
    sy.wave_len = 0;
    reverb_free(&sy.rv);
    fxdelay_free(&sy.dly);
    flanger_free(&sy.flg);
}

// CV matrix destination names (order = the SYM_* enum). Source conditioning +
// page + persistence all live in the shared cvmtx widget now.
const char *const synth_mtx_labels[SYM_N] = {
    "Cutoff", "Reso", "Timbre", "Env>Cut", "LFO Rate", "LFO Dep", "Level", "Pitch"
};

static void synth_process(int32_t out[MACHINE_BLOCK],
                          const int32_t in[MACHINE_BLOCK],
                          const machine_io_t *io)
{
    (void)in;
    static cvmed_t med[8];
    int cvm[8];
    for (int k = 0; k < 8; k++) cvm[k] = cvmed_step(&med[k], io->cv[k]);

    // FOUR macro knobs, K5..K8 = ch5..8 (cvm[4..7]). Each uses takeover: the
    // current value (Setup / default) holds until the knob is moved past a small
    // threshold, then the knob owns its param. This keeps the dev unit (weak
    // K5/K8) sounding right on defaults while built units get four live macros.
    float kn[4] = { (float)cvm[4]/4095.0f, (float)cvm[5]/4095.0f,
                    (float)cvm[6]/4095.0f, (float)cvm[7]/4095.0f };
    if (sy.knob_engine != sy.engine) {                // re-arm capture on an engine change
        sy.knob_engine = sy.engine;
        for (int i = 0; i < 4; i++) { sy.knob_capt[i] = kn[i]; sy.knob_live[i] = false; }
    }
    for (int i = 0; i < 4; i++)
        if (!sy.knob_live[i] && fabsf(kn[i] - sy.knob_capt[i]) > 0.03f) sy.knob_live[i] = true;

    // K6 = cutoff (full range, closes right down), K7 = resonance, K8 = env->cut
    if (sy.knob_live[1]) sy.cutoff_base = 10.0f * powf(600.0f, kn[1]);   // 10 Hz .. 6 kHz (log)
    if (sy.knob_live[2]) sy.res01 = kn[2];
    if (sy.knob_live[3]) sy.env_to_cut = kn[3];
    // K5 = engine-aware timbre: VA shape / FM index / WT fold
    if (sy.knob_live[0]) {
        if (sy.engine == ENG_FM)      sy.fm_index = kn[0] * 8.0f;
        else if (sy.engine == ENG_WT) sy.fold     = kn[0];
        else                          sy.shape    = kn[0];
    }
    // knob edits never reach the UI event queue: flag a committed move so the
    // autosave picks it up (hysteresis — live knobs track every block)
    {
        static float s_kdirty[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; i++)
            if (sy.knob_live[i] &&
                (s_kdirty[i] < 0 || fabsf(kn[i] - s_kdirty[i]) > 0.03f)) {
                s_kdirty[i] = kn[i];
                machine_state_dirty();
            }
    }
    sy.cv1_disp = cvm[0];
    // gate on TR1 (active low) or a web/soft MIDI note; computed up here so the
    // pitch holds through the release tail instead of snapping to the C3 fallback
    bool g = !(io->trig_level & 1) || audio_midi_gate();
    if (g) {                                          // freeze sy.freq while ungated
        if (audio_midi_gate()) {                      // web MIDI wins while held
            float n = (float)audio_midi_note();
            sy.freq = 440.0f * powf(2.0f, (n - 69.0f) / 12.0f);
        } else
            note_from_cv(cvm[0]);                     // CV1 = 1V/oct pitch
    }

    // ---- CV matrix: assigned CVs modulate params ON TOP of the knob/Setup base
    // (block-rate; median-conditioned; ch1/2 rescaled from their idle floor) ----
    cvmtx_track(&sy.mtx, cvm);                        // ch1/2 idle-floor follow
    float m_cut = 0, m_res = 0, m_tmb = 0, m_e2c = 0, m_lfr = 0, m_lfd = 0, m_lvl = 0, m_semi = 0;
    for (int d = 0; d < SYM_N; d++) {
        float av = cvmtx_val(&sy.mtx, cvm, d);        // amt * conditioned CV
        if (av == 0.0f) continue;
        switch (d) {
            case SYM_CUTOFF:   m_cut  += av * 4000.0f; break;
            case SYM_RES:      m_res  += av;           break;
            case SYM_TIMBRE:   m_tmb  += av;           break;
            case SYM_ENVCUT:   m_e2c  += av;           break;
            case SYM_LFORATE:  m_lfr  += av * 20.0f;   break;
            case SYM_LFODEPTH: m_lfd  += av;           break;
            case SYM_LEVEL:    m_lvl  += av;           break;
            case SYM_PITCH:    m_semi += av * 24.0f;   break;
        }
    }
    float cutoff_eff = sy.cutoff_base + m_cut;
    if (cutoff_eff < 8.0f) cutoff_eff = 8.0f;
    if (cutoff_eff > 6500.0f) cutoff_eff = 6500.0f;
    float res_eff = sy.res01 + m_res;       if (res_eff < 0) res_eff = 0; else if (res_eff > 1) res_eff = 1;
    float e2c_eff = sy.env_to_cut + m_e2c;  if (e2c_eff < 0) e2c_eff = 0; else if (e2c_eff > 1) e2c_eff = 1;
    float lvl_eff = sy.level + m_lvl;        if (lvl_eff < 0) lvl_eff = 0; else if (lvl_eff > 1.2f) lvl_eff = 1.2f;
    float lfr_eff = sy.lfo_rate + m_lfr;     if (lfr_eff < 0.01f) lfr_eff = 0.01f; else if (lfr_eff > 30.0f) lfr_eff = 30.0f;
    float lfd_eff = sy.lfo_depth + m_lfd;    if (lfd_eff < 0) lfd_eff = 0; else if (lfd_eff > 1) lfd_eff = 1;
    float pitch_mult = (m_semi != 0.0f) ? exp2f(m_semi / 12.0f) : 1.0f;
    float shape_eff = sy.shape + m_tmb;      if (shape_eff < 0) shape_eff = 0; else if (shape_eff > 1) shape_eff = 1;
    float fmidx_eff = sy.fm_index + m_tmb * 8.0f; if (fmidx_eff < 0) fmidx_eff = 0; else if (fmidx_eff > 12.0f) fmidx_eff = 12.0f;
    float fold_eff  = sy.fold + m_tmb;       if (fold_eff < 0) fold_eff = 0; else if (fold_eff > 1) fold_eff = 1;

    // gate on TR1 (active low); soft trigs from teleremote are already merged in
    // (g computed above so pitch can freeze through the release tail)
    if (g && !sy.gate)      sy.env_stage = ENV_ATK;   // note on (retrigger)
    else if (!g && sy.gate) sy.env_stage = ENV_REL;   // note off
    sy.gate = g;

    // per-block envelope increments (linear; clamp times so we never div by 0)
    float atk = sy.atk > 0.0005f ? sy.atk : 0.0005f;
    float dec = sy.dec > 0.0005f ? sy.dec : 0.0005f;
    float rel = sy.rel > 0.0005f ? sy.rel : 0.0005f;
    float atk_inc = 1.0f / (atk * SY_RATE);
    float dec_inc = (1.0f - sy.sus) / (dec * SY_RATE);
    float rel_inc = 1.0f / (rel * SY_RATE);

    // glide (portamento): slew the sounding frequency toward the target note
    if (sy.cur_freq <= 0.0f) sy.cur_freq = sy.freq;    // first note: snap, no glide from 0
    if (sy.glide > 0.0005f) {
        float blockdur = (float)(MACHINE_BLOCK / 2) / SY_RATE;   // one block of frames
        float coef = 1.0f - expf(-blockdur / sy.glide);
        sy.cur_freq += (sy.freq - sy.cur_freq) * coef;
    } else {
        sy.cur_freq = sy.freq;
    }
    // LFO (per block — sub-audio, so block granularity is smooth) -> pitch or cutoff
    float blockdur2 = (float)(MACHINE_BLOCK / 2) / (float)SY_RATE;
    sy.lfo_phase += lfr_eff * blockdur2;
    sy.lfo_phase -= (float)(int)sy.lfo_phase;
    float lfo = sinf(6.2831853f * sy.lfo_phase);
    float lfo_cut = (sy.lfo_dest == LFO_CUT)   ? (1.0f + lfo * lfd_eff * 0.8f) : 1.0f;
    float lfo_pit = (sy.lfo_dest == LFO_PITCH) ? exp2f(lfo * lfd_eff * 2.0f / 12.0f) : 1.0f;

    float dt = sy.cur_freq * lfo_pit * pitch_mult / SY_RATE;   // phase increment (+ vibrato + matrix pitch)
    if (dt > 0.5f) dt = 0.5f;                          // Nyquist guard
    float q = svf_damp(res_eff, 0.6f, 2.0f);          // 0..1 -> damping (2 = clean)

    // a NaN/Inf latched in the SVF is PERMANENT silence (NaN fails the output
    // clamp below, so every sample reads 0) — it looked like "FM killed the
    // audio". Recover the filter if a prior block blew it up. The real
    // prevention is the lower coefficient ceiling in svf_coef() below.
    if (!(fabsf(sy.flt_l.lp) < 1e9f) || !(fabsf(sy.flt_l.bp) < 1e9f))
        svf_reset(&sy.flt_l);

    int frames = MACHINE_BLOCK / 2;
    for (int f = 0; f < frames; f++) {
        // envelope
        switch (sy.env_stage) {
            case ENV_ATK: sy.env += atk_inc; if (sy.env >= 1.0f) { sy.env = 1.0f; sy.env_stage = ENV_DEC; } break;
            case ENV_DEC: sy.env -= dec_inc; if (sy.env <= sy.sus) { sy.env = sy.sus; sy.env_stage = ENV_SUS; } break;
            case ENV_SUS: sy.env = sy.sus; break;
            case ENV_REL: sy.env -= rel_inc; if (sy.env <= 0.0f) { sy.env = 0.0f; sy.env_stage = ENV_IDLE; } break;
            default:      sy.env = 0.0f; break;
        }

        // oscillator
        float osc;
        if (sy.engine == ENG_FM) {
            // 2-operator FM: carrier phase-modulated by a sine at ratio*freq,
            // modulation index scaled by the envelope (classic FM pluck/bell)
            const float TWO_PI = 6.2831853f;
            float mod = sinf(TWO_PI * sy.mphase);
            osc = sinf(TWO_PI * sy.phase + fmidx_eff * sy.env * mod);
            float mdt = dt * sy.fm_ratio;
            sy.mphase += mdt; sy.mphase -= (float)(int)sy.mphase;   // wrap 0..1
        } else if (sy.engine == ENG_WT && sy.wave && sy.wave_len > 1) {
            // wavetable: one phase traversal = one pass of the loaded wave, at the
            // note pitch (linear interpolation; no band-limiting -> some alias high up)
            float fpos = sy.phase * (float)sy.wave_len;
            int i0 = (int)fpos;
            float fr = fpos - (float)i0;
            if (i0 >= sy.wave_len) i0 = sy.wave_len - 1;
            int i1 = i0 + 1; if (i1 >= sy.wave_len) i1 = 0;
            osc = ((float)sy.wave[i0] + ((float)sy.wave[i1] - (float)sy.wave[i0]) * fr) / 32768.0f;
            if (fold_eff > 0.001f) {                  // knob7 wavefold: drive + reflect for a WT timbre sweep
                osc *= 1.0f + fold_eff * 4.0f;
                while (osc >  1.0f) osc =  2.0f - osc;
                while (osc < -1.0f) osc = -2.0f - osc;
            }
        } else {
            // polyBLEP saw <-> square morph
            float saw = 2.0f * sy.phase - 1.0f - polyblep(sy.phase, dt);
            float sq  = (sy.phase < 0.5f ? 1.0f : -1.0f) + polyblep(sy.phase, dt);
            float t2 = sy.phase + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
            sq -= polyblep(t2, dt);
            osc = saw + (sq - saw) * shape_eff;
        }
        sy.phase += dt; if (sy.phase >= 1.0f) sy.phase -= 1.0f;

        // filter: cutoff opened by the envelope + LFO wobble; SVF low-pass tap
        float fc = (cutoff_eff + sy.env * e2c_eff * 5000.0f) * lfo_cut;
        if (fc < 8.0f) fc = 8.0f;   // let the cutoff close nearly all the way down
        // 1.0 = Chamberlin stability ceiling (fc ~ SR/6); 1.5 let a bright,
        // high-energy FM tone drive the low-damping filter into self-oscillation
        // and blow up to NaN. Matches Drums' DR_FLT_FMAX.
        float coef = svf_coef(fc, SY_RATE, 1.0f);
        float lp;
        svf_step(&sy.flt_l, osc, coef, q, &lp, NULL, NULL);

        // VCA + scale to int16 with headroom + hard clamp
        float y = lp * sy.env * lvl_eff * 12000.0f;
        if (y > 32767.0f) y = 32767.0f; else if (y < -32768.0f) y = -32768.0f;
        int32_t s = ((int32_t)(int16_t)y) << 16;
        out[f * 2] = s;
        out[f * 2 + 1] = s;
    }

    // FX rack: generic slots in order -> reverb, float chain + soft limiter.
    fxrack_process_i32(&sy_rk, out, frames);
}

static cJSON *synth_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "eng", sy.engine);
    cJSON_AddStringToObject(o, "wave", sy.wave_name);
    cJSON_AddNumberToObject(o, "note", sy.base_note);
    cJSON_AddBoolToObject(o, "quant", sy.quantize);
    cJSON_AddNumberToObject(o, "shape", sy.shape);
    cJSON_AddNumberToObject(o, "fmr", sy.fm_ratio);
    cJSON_AddNumberToObject(o, "fmi", sy.fm_index);
    cJSON_AddNumberToObject(o, "atk", sy.atk);
    cJSON_AddNumberToObject(o, "dec", sy.dec);
    cJSON_AddNumberToObject(o, "sus", sy.sus);
    cJSON_AddNumberToObject(o, "rel", sy.rel);
    cJSON_AddNumberToObject(o, "e2c", sy.env_to_cut);
    cJSON_AddNumberToObject(o, "gld", sy.glide);
    fxrack_save(&sy_rk, o);   // slots + every effect param (shared FX rack)
    cJSON_AddNumberToObject(o, "lfr", sy.lfo_rate);
    cJSON_AddNumberToObject(o, "lfd", sy.lfo_depth);
    cJSON_AddNumberToObject(o, "lfx", sy.lfo_dest);
    cJSON_AddNumberToObject(o, "lvl", sy.level);
    // K6/K7/WT-fold state was knob-only (never persisted) until the autosave
    // sweep made knob edits savable — save what the save now fires for
    cJSON_AddNumberToObject(o, "cut", sy.cutoff_base);
    cJSON_AddNumberToObject(o, "res", sy.res01);
    cJSON_AddNumberToObject(o, "fold", sy.fold);
    cvmtx_save(&sy.mtx, o);                  // matrix ("mxs"/"mxa")
    return o;
}

static void synth_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "eng"))   && cJSON_IsNumber(j)) sy.engine = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "wave"))  && cJSON_IsString(j) && j->valuestring[0]) synth_load_wave(j->valuestring);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fmr"))   && cJSON_IsNumber(j)) sy.fm_ratio = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fmi"))   && cJSON_IsNumber(j)) sy.fm_index = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "note"))  && cJSON_IsNumber(j)) sy.base_note = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "quant")) && cJSON_IsBool(j))   sy.quantize = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "shape")) && cJSON_IsNumber(j)) sy.shape = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "atk"))   && cJSON_IsNumber(j)) sy.atk = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "dec"))   && cJSON_IsNumber(j)) sy.dec = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sus"))   && cJSON_IsNumber(j)) sy.sus = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rel"))   && cJSON_IsNumber(j)) sy.rel = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "e2c"))   && cJSON_IsNumber(j)) sy.env_to_cut = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "gld"))   && cJSON_IsNumber(j)) sy.glide = (float)j->valuedouble;
    fxrack_load(&sy_rk, node);   // slots + every effect param (shared FX rack)
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lfr"))   && cJSON_IsNumber(j)) sy.lfo_rate = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lfd"))   && cJSON_IsNumber(j)) sy.lfo_depth = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lfx"))   && cJSON_IsNumber(j)) sy.lfo_dest = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lvl"))   && cJSON_IsNumber(j)) sy.level = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cut")) && cJSON_IsNumber(j)) {
        float c = (float)j->valuedouble;
        sy.cutoff_base = c < 10.0f ? 10.0f : c > 6000.0f ? 6000.0f : c;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "res")) && cJSON_IsNumber(j)) {
        float r = (float)j->valuedouble;
        sy.res01 = r < 0 ? 0 : r > 1 ? 1 : r;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "fold")) && cJSON_IsNumber(j)) {
        float f = (float)j->valuedouble;
        sy.fold = f < 0 ? 0 : f > 1 ? 1 : f;
    }
    cvmtx_load(&sy.mtx, node);   // "mxs"/"mxa", with the legacy "msrc"/"mamt" fallback
}

// ---- named patch files: usr/synth/PAT_NNN.jsn (shared preset_store) --------
// The #23 mint/write/list/read code moved to components/util/preset_store
// when Keys became its second consumer; these wrappers keep the machine-local
// bits (the (de)serializers + the knob-takeover re-arm on load).
static const preset_store_t SY_PS = { "/sdcard/usr/synth", "PAT_" };

int synth_patch_save(char *id_out, size_t n)
{
    return preset_store_save(&SY_PS, synth_preset_save(), id_out, n);
}

// load a patch by id (0 ok). Re-arms knob takeover against the new values.
int synth_patch_load(const char *id)
{
    cJSON *root = preset_store_load(&SY_PS, id);
    if (!root) return -1;
    synth_preset_load(root);
    cJSON_Delete(root);
    sy.knob_engine = -1;
    return 0;
}

int synth_patch_list(char ids[][12], int max)
{
    return preset_store_list(&SY_PS, ids, max);
}

extern const machine_ui_t synth_menu_ui;

const machine_t machine_synth = {
    .name = "Synth",
    .start = synth_start,
    .stop = synth_stop,
    .process = synth_process,
    .preset_save = synth_preset_save,
    .preset_load = synth_preset_load,
    .ui = &synth_menu_ui,
};
