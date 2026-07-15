// Synth voice engine (see synth_priv.h). Monophonic subtractive voice:
// polyBLEP saw<->square osc -> SVF low-pass (env-modulated cutoff) -> ADSR VCA.
// Pitch on CV1 (1V/oct), gate on TR1. process() is pure DSP — no SD/heap/blocking.
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "machine.h"
#include "cvsmooth.h"
#include "svf.h"
#include "sample_ram.h"
#include "synth_priv.h"

sy_state_t sy;

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
    svf_reset(&sy.flt_l);
    return ESP_OK;
}

static void synth_stop(void)
{
    if (sy.wave) { heap_caps_free(sy.wave); sy.wave = NULL; }
    sy.wave_len = 0;
    reverb_free(&sy.rv);
}

static void synth_process(int32_t out[MACHINE_BLOCK],
                          const int32_t in[MACHINE_BLOCK],
                          const machine_io_t *io)
{
    (void)in;
    static cvmed_t med[8];
    int cvm[8];
    for (int k = 0; k < 8; k++) cvm[k] = cvmed_step(&med[k], io->cv[k]);

    // knobs 6/7 = cutoff / resonance (the two fully-good channels)
    float tc = (float)cvm[5] / 4095.0f;
    sy.cutoff_base = 30.0f * powf(200.0f, tc);        // 30 Hz .. 6 kHz (log)
    sy.res01 = (float)cvm[6] / 4095.0f;
    sy.cv1_disp = cvm[0];
    note_from_cv(cvm[0]);                             // CV1 = 1V/oct pitch

    // gate on TR1 (active low); soft trigs from teleremote are already merged in
    bool g = !(io->trig_level & 1);
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
    sy.lfo_phase += sy.lfo_rate * blockdur2;
    sy.lfo_phase -= (float)(int)sy.lfo_phase;
    float lfo = sinf(6.2831853f * sy.lfo_phase);
    float lfo_cut = (sy.lfo_dest == LFO_CUT)   ? (1.0f + lfo * sy.lfo_depth * 0.8f) : 1.0f;
    float lfo_pit = (sy.lfo_dest == LFO_PITCH) ? exp2f(lfo * sy.lfo_depth * 2.0f / 12.0f) : 1.0f;

    float dt = sy.cur_freq * lfo_pit / SY_RATE;         // phase increment (+ vibrato)
    if (dt > 0.5f) dt = 0.5f;                          // Nyquist guard
    float q = svf_damp(sy.res01, 0.6f, 2.0f);         // 0..1 knob -> damping (2 = clean)

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
            osc = sinf(TWO_PI * sy.phase + sy.fm_index * sy.env * mod);
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
        } else {
            // polyBLEP saw <-> square morph
            float saw = 2.0f * sy.phase - 1.0f - polyblep(sy.phase, dt);
            float sq  = (sy.phase < 0.5f ? 1.0f : -1.0f) + polyblep(sy.phase, dt);
            float t2 = sy.phase + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
            sq -= polyblep(t2, dt);
            osc = saw + (sq - saw) * sy.shape;
        }
        sy.phase += dt; if (sy.phase >= 1.0f) sy.phase -= 1.0f;

        // filter: cutoff opened by the envelope + LFO wobble; SVF low-pass tap
        float fc = (sy.cutoff_base + sy.env * sy.env_to_cut * 5000.0f) * lfo_cut;
        if (fc < 20.0f) fc = 20.0f;
        float coef = svf_coef(fc, SY_RATE, 1.5f);
        float lp;
        svf_step(&sy.flt_l, osc, coef, q, &lp, NULL, NULL);

        // VCA + scale to int16 with headroom + hard clamp
        float y = lp * sy.env * sy.level * 12000.0f;
        if (y > 32767.0f) y = 32767.0f; else if (y < -32768.0f) y = -32768.0f;
        int32_t s = ((int32_t)(int16_t)y) << 16;
        out[f * 2] = s;
        out[f * 2 + 1] = s;
    }

    // output reverb (equal-power wet/dry, in place) — AFTER the voice/filter
    if (sy.rv.mode != RV_OFF && sy.rv.slab)
        reverb_block_i32(&sy.rv, out, frames);
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
    cJSON_AddNumberToObject(o, "rv", sy.rv.mode);
    cJSON_AddNumberToObject(o, "rvmx", (int)(sy.rv.wet * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "lfr", sy.lfo_rate);
    cJSON_AddNumberToObject(o, "lfd", sy.lfo_depth);
    cJSON_AddNumberToObject(o, "lfx", sy.lfo_dest);
    cJSON_AddNumberToObject(o, "lvl", sy.level);
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
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rv"))    && cJSON_IsNumber(j)) {
        int m = j->valueint; if (m < 0 || m >= RV_N_MODES) m = RV_OFF;
        if (m != RV_OFF && !sy.rv.slab && reverb_init(&sy.rv) != ESP_OK) m = RV_OFF;
        reverb_set_mode(&sy.rv, m);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rvmx"))  && cJSON_IsNumber(j)) reverb_set_mix(&sy.rv, (float)j->valueint / 100.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lfr"))   && cJSON_IsNumber(j)) sy.lfo_rate = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lfd"))   && cJSON_IsNumber(j)) sy.lfo_depth = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lfx"))   && cJSON_IsNumber(j)) sy.lfo_dest = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lvl"))   && cJSON_IsNumber(j)) sy.level = (float)j->valuedouble;
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
