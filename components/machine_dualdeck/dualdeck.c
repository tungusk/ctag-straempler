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

#define DD_XF_GRAB 220        // knob counts of movement that grab the fader back

static volatile bool s_run = false, s_alive = false;

// ---- reader task (one task, both decks — sampler3's shared-reader pattern) ----
static void reader_serve(dd_deck_t *v, FILE **fp, char *cur, int16_t *chunk)
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
            v->wf_state = (*fp && v->file_frames) ? 1 : 0;
            v->wf_col = 0;
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
        sd_lock_take();
        fseek(*fp, sf_seek_pos(&v->sf, to), SEEK_SET);
        sd_lock_give();
        v->wpos = to;
        v->rpos_i = to;                   // reader is the ONLY seek-writer of rpos
        v->rpos_f = 0;
    }
    if (*fp && !v->track_req && !v->seek_req) {
        uint32_t lead = v->wpos - v->rpos_i;
        if (v->wpos < v->file_frames && lead < DD_RING_FRAMES - 4096) {
            uint32_t want = v->file_frames - v->wpos;
            if (want > 4096) want = 4096;
            sd_lock_take();
            size_t got = sampfile_read(*fp, &v->sf, chunk, want);
            sd_lock_give();
            if (got > 0) {
                uint32_t w = v->wpos % DD_RING_FRAMES;
                uint32_t first = DD_RING_FRAMES - w;
                if (first > got) first = got;
                memcpy(v->ring + w * 2, chunk, first * 4);
                if (first < got) memcpy(v->ring, chunk + first * 2, (got - first) * 4);
                v->wpos += got;
            }
        }
        if (v->loading && (v->wpos - v->rpos_i >= DD_LOW_WATER ||
                           v->wpos >= v->file_frames))
            v->loading = false;

        // waveform thumbnail, one column per pass once the ring is warm
        if (v->wf_state == 1 && v->file_frames &&
            (v->wpos - v->rpos_i >= DD_LOW_WATER || v->wpos >= v->file_frames)) {
            uint32_t p = (uint32_t)((uint64_t)v->wf_col * v->file_frames / DD_WF_W);
            uint32_t want = v->file_frames - p;
            if (want > 128) want = 128;
            sd_lock_take();
            long back = ftell(*fp);
            fseek(*fp, sf_seek_pos(&v->sf, p), SEEK_SET);
            size_t got = want ? sampfile_read(*fp, &v->sf, chunk, want) : 0;
            fseek(*fp, back, SEEK_SET);
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
    int16_t *chunk = heap_caps_malloc(4096 * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    s_alive = true;
    while (s_run) {
        reader_serve(&dd.d[0], &f[0], cur[0], chunk);
        reader_serve(&dd.d[1], &f[1], cur[1], chunk);
        vTaskDelay(1);   // >=1 tick (100 Hz): shorter is a busy-spin
    }
    for (int i = 0; i < 2; i++)
        if (f[i]) { sd_lock_take(); fclose(f[i]); sd_lock_give(); }
    free(chunk);
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

// ---- engine helpers ----------------------------------------------------------
// fire the armed transport ops for one deck (called ON the bar boundary, or
// immediately when free-running with no clock)
static void deck_fire(int i)
{
    dd_deck_t *v = &dd.d[i];
    if (v->arm_stop) {
        v->arm_stop = false;
        v->arm_start = false;
        v->playing = false;
        // re-park at the cue so the next start is instant
        v->loading = true;
        v->seek_to = (v->grid_offset < v->file_frames) ? v->grid_offset : 0;
        v->seek_req = true;
        return;
    }
    if (v->arm_start) {
        v->arm_start = false;
        if (!v->track[0] || !v->file_frames) return;
        if (v->playing || v->rpos_i != v->grid_offset || v->loading) {
            // restart / not parked: full seek protocol (brief refill mute)
            v->loading = true;
            v->seek_to = (v->grid_offset < v->file_frames) ? v->grid_offset : 0;
            v->seek_req = true;
        }
        v->phase_int = 0;
        v->playing = true;
        // takeover fade: pull the mix toward the entering deck (Arlo: YES);
        // knob movement past the grab deadband takes it back (soft pickup)
        if (dd.fade_beats > 0 && dd.ci.clk.locked && dd.ci.clk.period > 0) {
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
        float p_trk = fmodf((float)((int64_t)v->rpos_i - (int64_t)v->grid_offset), seg_tf) / seg_tf;
        if (p_trk < 0) p_trk += 1.0f;
        #define DD_LAG_LEAD_FR (0.0131f * 44100.0f)   // deck's measured output-chain lead
        float lead = DD_LAG_LEAD_FR / (float)dd.ci.clk.period;
        float err = p_ext - p_trk + lead;
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
    dd.fade_beats = 4;
    dd.xf = 0.0f;
    dd.manual = true;
    clockin_reset(&dd.ci, dd_ppb[dd.ppb_idx]);
    s_run = true;
    xTaskCreate(reader_task, "dd_reader", 4096, NULL, 6, NULL);
    audio_status_set_voices("dualdeck", "");
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

    // ---- trig gates on the shared grammar resolver (trig_gate.h): TR1 =
    // deck A, TR2 = deck B. Tap = quantized start/restart (on release);
    // TG_HOLD at the 0.6 s threshold = quantized stop (release then no-ops).
    static trig_gate_t tg[2];
    for (int i = 0; i < 2; i++) {
        tg_event_t e = trig_gate_step(&tg[i], !(io->trig_level & (1 << i)), frames);
        if (e == TG_HOLD) dualdeck_arm_stop(i);
        else if (e == TG_REL_SHORT) dualdeck_arm_start(i);
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

    // ---- crossfade: three states — AUTO (takeover fade in flight), HELD
    // (fade landed; the mix stays put wherever automation left it), MANUAL
    // (knob is live). Moving the knob past the grab deadband promotes
    // auto/held to manual — the pickup. Handing straight back to manual on
    // fade completion would snap the mix to wherever the knob happens to
    // sit, defeating the takeover entirely (caught on first bench test).
    dd.xf_cv = io->cv[5];
    if (!dd.manual) {                  // auto or held: watch for the grab.
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
    } else if (dd.manual) {
        float xf_target = (float)dd.xf_cv / 4095.0f;
        // gentle slew so a jumpy ADC read never steps the mix
        dd.xf += 0.2f * (xf_target - dd.xf);
    }
    if (dd.xf < 0) dd.xf = 0;
    if (dd.xf > 1) dd.xf = 1;
    // equal-power gains
    float ga = cosf(dd.xf * (float)M_PI_2);
    float gb = sinf(dd.xf * (float)M_PI_2);

    // ---- master DJ filter from knob7 (the deck's knob6 mapping, verbatim)
    int fcv = io->cv[6];
    dd.filt_cv = fcv;
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
            uint32_t avail_to = v->wpos < v->file_frames ? v->wpos : v->file_frames;
            bool can_play = v->playing && !v->loading && v->rpos_i + 1 < avail_to;
            if (!can_play && v->playing && !v->loading && !v->seek_req &&
                v->file_frames && v->rpos_i + 1 < v->file_frames)
                starved[i] = true;
            float gt = can_play ? 1.0f : 0.0f;
            v->out_gain += (gt - v->out_gain) * 0.015f;
            float l, r;
            if (!can_play) {
                if (v->playing && !v->loading && v->file_frames &&
                    v->rpos_i + 1 >= v->file_frames && !v->seek_req) {
                    // EOF: wrap to the cue (decks loop by design)
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
                v->rpos_f += rate[i];
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
        dd.fade_beats = (fb == 0 || fb == 1 || fb == 4 || fb == 8) ? fb : 4;
    }
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
    .name = "DualDeck",
    .start = dualdeck_start,
    .stop = dualdeck_stop,
    .process = dualdeck_process,
    .preset_save = dualdeck_preset_save,
    .preset_load = dualdeck_preset_load,
    .ui = &dualdeck_menu_ui,
};
