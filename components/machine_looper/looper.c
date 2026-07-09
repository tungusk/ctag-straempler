// M2 looper engine — 4 mono PSRAM tracks, CV-clock-synced bar-quantized
// recording. Runs entirely in the audio task's process() callback; the UI
// task pokes commands via the shared lp state (see looper_priv.h).
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "fileio.h"
#include "machine.h"
#include "clock.h"
#include "audio.h"
#include "looper_priv.h"

lp_state_t lp;

// ---- clock detection ------------------------------------------------------
// tempo detection via the shared core detector (components/machine/clock.c);
// lp.bpm / lp.locked mirror it for the UI. "period" is samples per quarter.
static beatclock_t s_clk;

// clock line as a 0/4095 level fed to the detector, from either a CV channel
// (raw, thresholded in clock.c) or a trig input. Trigs are active-low: a clock
// pulse pulls the line low, mapped to the "high" clock state.
static uint16_t clock_level(const machine_io_t *io)
{
    if (lp.clk_src == LP_CLK_TR1) return (io->trig_level & 1) ? 0 : 4095;
    if (lp.clk_src == LP_CLK_TR2) return (io->trig_level & 2) ? 0 : 4095;
    return io->cv[lp.clk_src & 7];
}

// ---- track helpers --------------------------------------------------------
static uint32_t bar_frames(void)
{
    if (!s_clk.period) return 0;
    return s_clk.period * 4u * (uint32_t)(lp.bars > 0 ? lp.bars : 1);
}

static void track_start_record(lp_track_t *t)
{
    t->pos = 0;
    t->len = 0;
    // synced: auto-stop after N bars; unsynced: cap at buffer length
    uint32_t bf = (lp.sync_on && lp.locked) ? bar_frames() : 0;
    if (bf == 0 || bf > LP_BUF_FRAMES) bf = LP_BUF_FRAMES;
    t->target = bf;
    t->state = LP_REC;
}

// ---- lifecycle ------------------------------------------------------------
static esp_err_t looper_start(void)
{
    memset(&lp, 0, sizeof(lp));
    lp.sync_on = true;
    lp.clk_src = 7;   // CV8 by default — frees both trigs for buttons
    lp.bars = 4;
    lp.sel = 0;
    for (int i = 0; i < LP_TRACKS; i++) {
        lp.tr[i].buf = heap_caps_malloc(LP_BUF_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!lp.tr[i].buf) {
            ESP_LOGE("LOOPER", "PSRAM alloc failed for track %d", i);
            return ESP_ERR_NO_MEM;
        }
        lp.tr[i].state = LP_EMPTY;
        lp.tr[i].vol = 255;      // unity
        lp.tr[i].pan = 2048;     // center
        lp.tr[i].cutoff = 4095;  // filter open (bright)
        lp.tr[i].res = 900;
        lp.tr[i].svf_low = lp.tr[i].svf_band = 0.0f;
    }
    clock_reset(&s_clk);
    audio_status_set_voices("looper", "");
    return ESP_OK;
}

static void looper_stop(void)
{
    for (int i = 0; i < LP_TRACKS; i++) {
        free(lp.tr[i].buf);
        lp.tr[i].buf = NULL;
    }
}

// consume a queued UI command for track i
static void apply_cmd(int i)
{
    lp_track_t *t = &lp.tr[i];
    if (lp.cmd_clear[i]) {
        lp.cmd_clear[i] = 0;
        t->state = LP_EMPTY;
        t->len = t->pos = t->target = 0;
    }
    if (lp.cmd_action[i]) {
        lp.cmd_action[i] = 0;
        switch (t->state) {
            case LP_EMPTY:
                // arm: wait for the next bar boundary when synced, else record now
                if (lp.sync_on && lp.locked) t->state = LP_ARMED;
                else track_start_record(t);
                break;
            case LP_ARMED:  t->state = LP_EMPTY; break;    // cancel arm
            case LP_REC:    t->len = t->pos; t->pos = 0; t->state = LP_PLAY; break; // punch out early
            case LP_PLAY:   t->state = LP_STOP; break;
            case LP_STOP:
                // re-arm: clear the loop and record over it (the encoder-only
                // way to redo a track). Play/stop cycle: PLAY -> STOP -> re-arm
                t->len = 0; t->pos = 0;
                if (lp.sync_on && lp.locked) t->state = LP_ARMED;
                else track_start_record(t);
                break;
        }
    }
}

// ---- audio ---------------------------------------------------------------
// in/out are interleaved stereo int32 (left-justified); one MACHINE_BLOCK =
// 32 stereo frames. Mix all playing tracks to mono, fan out to both channels.
static void looper_process(int32_t out[MACHINE_BLOCK],
                           const int32_t in[MACHINE_BLOCK],
                           const machine_io_t *io)
{
    for (int i = 0; i < LP_TRACKS; i++) apply_cmd(i);

    // TR inputs are ACTIVE LOW: idle reads high (bit set), a gate pulls low.
    // Detect the falling edge (1 -> 0), prev seeded idle-high so a fresh start
    // sees no phantom edge. TR1 = context action (arm/rec/punch/stop/re-arm),
    // TR2 = play/stop on the selected lane. A trig used as the clock source is
    // masked so it doesn't double as a button.
    uint8_t clk_mask = (lp.clk_src == LP_CLK_TR1) ? 1 :
                       (lp.clk_src == LP_CLK_TR2) ? 2 : 0;
    static uint8_t prev_trig = 0x03;
    uint8_t pressed = prev_trig & (~io->trig_level) & 0x03 & ~clk_mask;
    prev_trig = io->trig_level;
    if (pressed & 1) lp.cmd_action[lp.sel] = 1;
    if (pressed & 2) {
        lp_track_t *t = &lp.tr[lp.sel];
        if (t->state == LP_PLAY) t->state = LP_STOP;
        else if (t->len > 0) { t->pos = 0; t->state = LP_PLAY; }
    }

    // the two good knobs shape the selected track: CV6 = level, CV7 = pan.
    // (knobs 5/8 are faulty on this unit, so per-track-fixed mapping is out;
    // this is a focus-style control — values persist per track when deselected)
    lp.tr[lp.sel].vol = io->cv[5] >> 4;   // CV6 -> 0..255
    lp.tr[lp.sel].pan = io->cv[6];        // CV7 -> 0..4095

    // filter mod on the jacks: rising CV1 OPENS the selected track's cutoff
    // (patch an envelope/LFO to open it), CV2 raises resonance. The 1V/oct
    // jacks idle ~880/4095 (below the 900 floor). Only overwrite the track's
    // stored setting when the jack is actually driven (c > 0) — so each track
    // REMEMBERS its cutoff/res and an unpatched jack doesn't slam it on select.
    if (lp.filter_on) {
        uint16_t c1 = io->cv[0] > 900 ? io->cv[0] - 900 : 0;   // 0..3195
        uint16_t c2 = io->cv[1] > 900 ? io->cv[1] - 900 : 0;
        if (c1) lp.tr[lp.sel].cutoff = 295 + (uint16_t)((uint32_t)c1 * 3800 / 3195);
        if (c2) lp.tr[lp.sel].res    = 900 + (uint16_t)((uint32_t)c2 * 3000 / 3195);
    }

    // per-block SVF coeffs for every track (cheap: 4 sinf per 1.45ms block)
    for (int i = 0; i < LP_TRACKS; i++) {
        float fc = 120.0f + ((float)lp.tr[i].cutoff / 4095.0f) * 5800.0f;  // Hz
        float f = 2.0f * sinf((float)M_PI * fc / (float)LP_RATE);
        if (f > 0.95f) f = 0.95f;
        lp.tr[i].f = f;
        lp.tr[i].q = 2.0f - ((float)lp.tr[i].res / 4095.0f) * 1.9f;        // 2.0..0.1
    }

    int frames = MACHINE_BLOCK / 2;
    uint16_t clk = clock_level(io);

    for (int f = 0; f < frames; f++) {
        bool bar_edge = false;
        if (clock_tick(&s_clk, clk)) {
            lp.bpm = s_clk.bpm;             // mirror to the UI-facing fields
            lp.locked = s_clk.locked;
            // each clock pulse = one quarter; a bar (4/4) is every 4 pulses.
            // Armed tracks start on the next bar boundary; the record LENGTH
            // (bars) is handled separately by track_start_record's target.
            if (s_clk.locked && (s_clk.ring_n % 4) == 0)
                bar_edge = true;
        }
        if (!s_clk.locked) lp.locked = false;   // reflect clock-stop promptly

        // mono input = (L+R)/2 from the 32-bit left-justified samples
        int32_t l = in[f * 2] >> 16;
        int32_t r = in[f * 2 + 1] >> 16;
        int16_t mono_in = (int16_t)((l + r) / 2);

        int32_t mixL = 0, mixR = 0;
        for (int i = 0; i < LP_TRACKS; i++) {
            lp_track_t *t = &lp.tr[i];

            if (t->state == LP_ARMED && bar_edge)
                track_start_record(t);

            if (t->state == LP_REC) {
                t->buf[t->pos] = mono_in;
                t->pos++;
                if (t->pos >= t->target) {
                    t->len = t->pos;
                    t->pos = 0;
                    t->state = LP_PLAY;
                }
            } else if (t->state == LP_PLAY && t->len > 0) {
                int32_t raw = t->buf[t->pos];
                if (lp.filter_on) {                                    // bandpass SVF
                    t->svf_low += t->f * t->svf_band;
                    float high = (float)raw - t->svf_low - t->q * t->svf_band;
                    t->svf_band += t->f * high;
                    raw = (int32_t)t->svf_band;
                }
                int32_t s = (raw * t->vol) >> 8;                       // level
                mixL += (s * (4095 - t->pan)) >> 12;                   // linear pan
                mixR += (s * t->pan) >> 12;
                t->pos++;
                if (t->pos >= t->len) t->pos = 0;
            }
        }

        if (lp.monitor) { mixL += mono_in; mixR += mono_in; }

        if (mixL > 32767) mixL = 32767; else if (mixL < -32768) mixL = -32768;
        if (mixR > 32767) mixR = 32767; else if (mixR < -32768) mixR = -32768;
        out[f * 2]     = mixL << 16;
        out[f * 2 + 1] = mixR << 16;
    }
}

// Save a track's RAM loop to the SD library as LOOP_NNNN.RAW + .JSN sidecar,
// in the same stereo-packed int32 format the recording service writes (mono
// duplicated L=R), so the samplers can load it and it survives reboot. Runs on
// the UI task (explicit action); playback keeps reading the buffer read-only.
int looper_save_track(int i)
{
    if (i < 0 || i >= LP_TRACKS) return -1;
    lp_track_t *t = &lp.tr[i];
    if (t->len == 0 || !t->buf) return -1;

    char name[16], path[48];
    struct stat st;
    for (int n = 0; n < 9999; n++) {
        snprintf(name, sizeof(name), "LOOP_%04d", n);
        snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", name);
        if (stat(path, &st) != 0) break;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE("LOOPER", "save: cannot open %s", path); return -1; }

    int32_t *chunk = heap_caps_malloc(512 * sizeof(int32_t), MALLOC_CAP_INTERNAL);
    if (!chunk) { fclose(f); return -1; }
    uint32_t pos = 0, left = t->len;
    while (left) {
        int nfr = left > 512 ? 512 : (int)left;
        for (int k = 0; k < nfr; k++) {
            uint16_t s = (uint16_t)t->buf[pos + k];
            chunk[k] = ((uint32_t)s << 16) | s;   // mono -> L=R
        }
        fwrite(chunk, sizeof(int32_t), nfr, f);
        pos += nfr; left -= nfr;
    }
    free(chunk);
    fclose(f);

    char jsn[48], field[24];
    snprintf(jsn, sizeof(jsn), "/sdcard/usr/%s.JSN", name);
    cJSON *root = cJSON_CreateObject();
    snprintf(field, sizeof(field), "%s.raw", name);
    cJSON_AddStringToObject(root, "name", field);
    cJSON_AddStringToObject(root, "id", name);
    cJSON_AddStringToObject(root, "description", "Looper capture");
    cJSON_AddStringToObject(root, "tags_s", "loop");
    cJSON_AddStringToObject(root, "username", "myself");
    cJSON_AddStringToObject(root, "url", "local");
    cJSON_AddStringToObject(root, "license", "own license");
    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    if (s) { writeJSONFile(jsn, s); free(s); }
    ESP_LOGI("LOOPER", "saved track %d -> %s (%lu frames)", i, name, (unsigned long)t->len);
    return 0;
}

// Bounce all playing tracks down into track 1 (index 0): sum every
// contributing loop to mono, baking in each track's level and (when the BP
// filter is on) its bandpass, then clear tracks 2-4 so they're free to overdub
// on top. Length = the longest contributing loop; shorter loops wrap to fill
// it (sample-exact when lengths are bar multiples of each other, the normal
// synced case). Pan is dropped — the destination is a mono track. Runs on the
// UI task (explicit action). The audio task keeps playing the old mix until we
// swap; sources are read-only here so concurrent playback is safe.
int looper_bounce(void)
{
    // refuse while any track is capturing — its buf/len/state are in flux
    for (int i = 0; i < LP_TRACKS; i++)
        if (lp.tr[i].state == LP_REC || lp.tr[i].state == LP_ARMED) return -2;

    // gather contributing tracks (playing/stopped with content) + the span
    uint32_t bounce_len = 0;
    int n_src = 0;
    for (int i = 0; i < LP_TRACKS; i++) {
        lp_track_t *t = &lp.tr[i];
        if (t->len > 0 && (t->state == LP_PLAY || t->state == LP_STOP)) {
            if (t->len > bounce_len) bounce_len = t->len;
            n_src++;
        }
    }
    if (n_src == 0 || bounce_len == 0) return -1;

    int16_t *scratch = heap_caps_malloc(bounce_len * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!scratch) return -1;

    // per-track running bandpass state, advanced in playback (wrap) order so
    // the baked filter tracks what the engine renders at looper.c's play path
    float bl[LP_TRACKS] = {0}, bb[LP_TRACKS] = {0};

    for (uint32_t j = 0; j < bounce_len; j++) {
        int32_t acc = 0;
        for (int i = 0; i < LP_TRACKS; i++) {
            lp_track_t *t = &lp.tr[i];
            if (t->len == 0 || !(t->state == LP_PLAY || t->state == LP_STOP)) continue;
            int32_t raw = t->buf[j % t->len];
            if (lp.filter_on) {                                    // bandpass SVF
                bl[i] += t->f * bb[i];
                float high = (float)raw - bl[i] - t->q * bb[i];
                bb[i] += t->f * high;
                raw = (int32_t)bb[i];
            }
            acc += (raw * t->vol) >> 8;                            // level
        }
        if (acc > 32767) acc = 32767; else if (acc < -32768) acc = -32768;
        scratch[j] = (int16_t)acc;
    }

    // swap in: silence all lanes, let the audio task drain a few blocks (it
    // stops reading a track the frame its state != PLAY), then install the
    // bounce on track 1 at unity/center and leave 2-4 empty
    for (int i = 0; i < LP_TRACKS; i++) {
        lp.tr[i].state = LP_EMPTY;
        lp.tr[i].len = lp.tr[i].pos = lp.tr[i].target = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    lp_track_t *d = &lp.tr[0];
    memcpy(d->buf, scratch, bounce_len * sizeof(int16_t));
    free(scratch);
    d->len = bounce_len;
    d->pos = 0;
    d->vol = 255;                 // unity — the balance is already baked in
    d->pan = 2048;                // center
    d->svf_low = d->svf_band = 0.0f;
    d->state = LP_PLAY;

    ESP_LOGI("LOOPER", "bounced %d track(s) -> track 1 (%lu frames)",
             n_src, (unsigned long)bounce_len);
    return 0;
}

// persist the machine settings (not the RAM loops) so the looper remembers
// its configuration across machine switches and reboots
static cJSON *looper_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "sync", lp.sync_on);
    cJSON_AddNumberToObject(o, "clk_src", lp.clk_src);
    cJSON_AddNumberToObject(o, "bars", lp.bars);
    cJSON_AddBoolToObject(o, "monitor", lp.monitor);
    cJSON_AddBoolToObject(o, "filter", lp.filter_on);
    return o;
}

static void looper_preset_load(const cJSON *node)
{
    if (!node) return;   // no saved state — keep start() defaults
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sync")))                     lp.sync_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk_src")) && cJSON_IsNumber(j)) lp.clk_src = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "bars")) && cJSON_IsNumber(j))    lp.bars = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "monitor")))                  lp.monitor = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "filter")))                   lp.filter_on = cJSON_IsTrue(j);
}

extern const machine_ui_t looper_menu_ui;

const machine_t machine_looper = {
    .name = "Looper",
    .start = looper_start,
    .stop = looper_stop,
    .process = looper_process,
    .preset_save = looper_preset_save,
    .preset_load = looper_preset_load,
    .ui = &looper_menu_ui,
    // audio loops stay RAM-only (save-to-library is the explicit gesture)
};
