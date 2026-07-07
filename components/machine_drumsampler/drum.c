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

static void trigger_pad(dr_pad_t *p, uint8_t vel)
{
    if (!p->enabled || p->len == 0) return;
    if (p->playing) {
        p->vel_next = vel;      // fade the running voice out, then restart
        p->fade = 256;
        p->retrig = true;
    } else {
        p->pos = 0;
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
    dr.n_pads = 8;
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
            uint16_t sv = io->cv[dr.sel_src[t] & 7];
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
        for (int f = 0; f < frames; f++) {
            if (p->retrig) {
                // ~0.7 ms fade of the old voice, then restart at the new hit
                p->fade -= 8;
                if (p->fade <= 0) {
                    p->retrig = false;
                    p->pos = 0;
                    p->vel = p->vel_next;
                    continue;
                }
            }
            uint32_t pos = p->pos;
            if (pos >= len) { p->playing = false; break; }
            int env;
            if (p->retrig) {
                env = p->fade;
            } else {
                env = 256;                                  // declick ramps:
                if (pos < 64) env = (int)pos << 2;          // ~1.5 ms attack
                uint32_t rem = len - pos;
                if (rem < 256 && (int)rem < env) env = (int)rem;   // ~6 ms tail
            }
            int s = (p->buf[pos] * env) >> 8;
            int g  = ((int)p->level * (int)p->vel) >> 8;    // 0..255
            accL[f] += (s * ((g * (255 - p->pan)) >> 8)) >> 8;
            accR[f] += (s * ((g * p->pan) >> 8)) >> 8;
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
int drum_load_pad(int pad, const char *name)
{
    if (pad < 0 || pad >= DR_PADS || !dr.pad[pad].buf) return -1;
    dr_pad_t *p = &dr.pad[pad];
    p->playing = false;
    p->retrig = false;
    p->len = 0;                      // engine stops reading before we overwrite
    uint32_t n = sample_load(name, p->buf, DR_MAX_FRAMES, true);   // mono
    if (n == 0) { p->sample[0] = 0; return -1; }
    strncpy(p->sample, name, sizeof(p->sample) - 1);
    p->sample[sizeof(p->sample) - 1] = 0;
    p->len = n;
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
}

// ---- preset -----------------------------------------------------------------
static cJSON *drum_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "pads", dr.n_pads);
    cJSON_AddBoolToObject(o, "cvsel", dr.cv_select);
    cJSON_AddBoolToObject(o, "vel", dr.velocity);
    cJSON_AddNumberToObject(o, "sens", dr.sens);
    cJSON_AddNumberToObject(o, "sel0", dr.sel_src[0]);
    cJSON_AddNumberToObject(o, "sel1", dr.sel_src[1]);
    cJSON *pads = cJSON_AddArrayToObject(o, "pad");
    for (int i = 0; i < DR_PADS; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "s", dr.pad[i].sample);
        cJSON_AddNumberToObject(p, "lvl", dr.pad[i].level);
        cJSON_AddNumberToObject(p, "pan", dr.pad[i].pan);
        cJSON_AddBoolToObject(p, "en", dr.pad[i].enabled);
        cJSON_AddNumberToObject(p, "src", dr.pad[i].trig_src + 1);   // CV number, 1-based
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
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sens")) && cJSON_IsNumber(j)) {
        dr.sens = j->valueint;
        if (dr.sens < 0) dr.sens = 0;
        if (dr.sens > 2) dr.sens = 2;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sel0")) && cJSON_IsNumber(j)) dr.sel_src[0] = j->valueint & 7;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sel1")) && cJSON_IsNumber(j)) dr.sel_src[1] = j->valueint & 7;
    cJSON *pads = cJSON_GetObjectItemCaseSensitive(node, "pad");
    if (cJSON_IsArray(pads)) {
        int i = 0;
        cJSON *p;
        cJSON_ArrayForEach(p, pads) {
            if (i >= DR_PADS) break;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "lvl")) && cJSON_IsNumber(j)) dr.pad[i].level = (uint8_t)j->valueint;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "pan")) && cJSON_IsNumber(j)) dr.pad[i].pan = (uint8_t)j->valueint;
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "en")))                       dr.pad[i].enabled = cJSON_IsTrue(j);
            if ((j = cJSON_GetObjectItemCaseSensitive(p, "src")) && cJSON_IsNumber(j)) dr.pad[i].trig_src = (j->valueint - 1) & 7;
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
