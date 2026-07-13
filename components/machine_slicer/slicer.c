// M3 slicer engine — one stereo PSRAM sample, equal-grid slices, one
// monophonic retrigger voice. Runs in the audio task's process() callback;
// the UI task loads samples and pokes command flags (see slicer_priv.h).
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sample_ram.h"
#include "machine.h"
#include "audio.h"
#include "slicer_priv.h"

sl_state_t sl;

// ---- slicing ---------------------------------------------------------------
static void recompute_grid(int n)
{
    if (n < 1) n = 16;   // Auto has no meaning for an even grid — use 16
    if (n > SL_MAX_SLICES) n = SL_MAX_SLICES;
    sl.n_slices = n;
    for (int i = 0; i <= n; i++)
        sl.slice_pt[i] = (uint32_t)((uint64_t)i * sl.len / n);
}

// transient detector: window energy -> onset (rectified energy rise) -> pick
// the strongest min-spaced onsets as slice boundaries. The energy envelope is
// computed once on load (compute_envelope); pick_transients re-runs cheaply on
// sensitivity/count changes so the dial-in screen is responsive.
#define SL_WIN     512
#define SL_MAXCAND 192   // 2x the pool so 128 transient slices aren't starved
static float    s_env[SL_MAX_FRAMES * 2 / SL_WIN + 2];   // sized for mono (2x frames)
static uint32_t s_nwin = 0;

static void compute_envelope(void)
{
    s_nwin = sl.len / SL_WIN;
    for (uint32_t w = 0; w < s_nwin; w++) {
        uint32_t a = w * SL_WIN;
        uint64_t acc = 0;
        for (int k = 0; k < SL_WIN; k++) {
            uint32_t fi = a + k;
            int l = sl.mono ? sl.buf[fi] : sl.buf[fi * 2];     if (l < 0) l = -l;
            int r = sl.mono ? l          : sl.buf[fi * 2 + 1]; if (r < 0) r = -r;
            acc += (uint32_t)(l + r);
        }
        s_env[w] = (float)acc;
    }
}

// target: 0 = Auto (keep every detected transient), else a max slice count.
// sl.sensitivity (0..100) scales the onset threshold: higher = more slices.
static void pick_transients(int target)
{
    uint32_t nwin = s_nwin;
    if (nwin < 4) { recompute_grid(target); return; }

    // threshold relative to the LOUDEST onset — maps sensitivity monotonically
    // to how far below the peak to include (few near the top, many near 0)
    float maxo = 1.0f;
    for (uint32_t w = 2; w < nwin; w++) { float o = s_env[w] - s_env[w - 1]; if (o > maxo) maxo = o; }
    float thresh = maxo * (1.0f - (float)sl.sensitivity / 100.0f * 0.97f);
    if (thresh < maxo * 0.02f) thresh = maxo * 0.02f;
    uint32_t min_gap = (SL_RATE / 12) / SL_WIN;   // ~80 ms minimum slice
    if (min_gap < 1) min_gap = 1;

    float cs[SL_MAXCAND]; uint32_t cp[SL_MAXCAND]; int nc = 0;
    uint32_t last = 0; bool have_last = false;
    for (uint32_t w = 2; w < nwin - 1 && nc < SL_MAXCAND; w++) {
        float o = s_env[w] - s_env[w - 1];
        if (o > thresh && o >= (s_env[w - 1] - s_env[w - 2]) && o > (s_env[w + 1] - s_env[w])) {
            if (have_last && w - last < min_gap) continue;
            cs[nc] = o; cp[nc] = w * SL_WIN; nc++; last = w; have_last = true;
        }
    }
    if (nc == 0) { recompute_grid(target); return; }

    for (int i = 1; i < nc; i++) {   // sort candidates by strength desc
        float ks = cs[i]; uint32_t kp = cp[i]; int j = i - 1;
        while (j >= 0 && cs[j] < ks) { cs[j + 1] = cs[j]; cp[j + 1] = cp[j]; j--; }
        cs[j + 1] = ks; cp[j + 1] = kp;
    }
    int want = (target <= 0) ? nc : target - 1;   // Auto keeps all detected
    if (want > nc) want = nc;
    if (want > SL_MAX_SLICES - 1) want = SL_MAX_SLICES - 1;

    uint32_t pts[SL_MAX_SLICES];
    for (int i = 0; i < want; i++) pts[i] = cp[i];
    for (int i = 1; i < want; i++) {  // sort chosen positions ascending
        uint32_t k = pts[i]; int j = i - 1;
        while (j >= 0 && pts[j] > k) { pts[j + 1] = pts[j]; j--; }
        pts[j + 1] = k;
    }
    sl.slice_pt[0] = 0;
    for (int i = 0; i < want; i++) sl.slice_pt[i + 1] = pts[i];
    sl.slice_pt[want + 1] = sl.len;
    sl.n_slices = want + 1;
}

static void recompute_slices(void)
{
    if (sl.len == 0) { sl.n_slices = 1; sl.slice_pt[0] = 0; sl.slice_pt[1] = 0; return; }
    if (sl.ot_active && sl.ot_present && sl.ot_n > 0) {        // Octatrack sidecar
        memcpy(sl.slice_pt, sl.ot_pt, (size_t)(sl.ot_n + 1) * sizeof(uint32_t));
        sl.n_slices = sl.ot_n;
    }
    else if (sl.transient_mode) pick_transients(sl.slice_target);  // cached envelope
    else                        recompute_grid(sl.slice_target);
    if (sl.sel >= sl.n_slices) sl.sel = sl.n_slices - 1;
}

void slicer_reslice(void)
{
    sl.loading = true;      // briefly mute — slice_pt[] is being rewritten
    sl.playing = false;
    recompute_slices();
    sl.loading = false;
}

// ---- playback -------------------------------------------------------------
static void fire_slice(int s)
{
    if (sl.len == 0 || sl.n_slices < 1) return;
    if (s < 0) s = 0;
    if (s >= sl.n_slices) s = sl.n_slices - 1;
    sl.s_start = sl.slice_pt[s];
    sl.s_end = sl.slice_pt[s + 1];
    if (sl.s_end > sl.len || sl.s_end <= sl.s_start) sl.s_end = sl.len;
    sl.cur = s;
    sl.pos = sl.reverse ? (double)(sl.s_end - 1) : (double)sl.s_start;
    sl.playing = true;
}

static void end_slice(void)
{
    sl.playing = false;
    if (sl.auto_on)                      // walk to the next slice
        fire_slice((sl.cur + 1) % sl.n_slices);
}

// ---- lifecycle ------------------------------------------------------------
static esp_err_t slicer_start(void)
{
    memset(&sl, 0, sizeof(sl));
    // descending alloc: take the biggest slab PSRAM will actually grant
    static const int sl_secs[] = {SL_MAX_SECS, 14, 12};
    sl.cap_frames = 0;
    for (int i = 0; i < 3 && !sl.buf; i++) {
        sl.cap_frames = (uint32_t)SL_RATE * (uint32_t)sl_secs[i];
        sl.buf = heap_caps_malloc((size_t)sl.cap_frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (sl.buf) ESP_LOGI("SLICER", "sample slab %lu s stereo",
                         (unsigned long)(sl.cap_frames / SL_RATE));
    if (!sl.buf) { ESP_LOGE("SLICER", "PSRAM alloc failed"); return ESP_ERR_NO_MEM; }
    sl.slice_target = 16;
    sl.n_slices = 16;
    sl.transient_mode = false;
    sl.sensitivity = 50;
    sl.level = 255;
    sl.pitch_cv = 2048;
    sl.inc = 1.0f;
    sl.loading = false;

    char first[1][24];
    if (slicer_list_samples(first, 1) > 0) slicer_load(first[0]);
    audio_status_set_voices("slicer", "");
    return ESP_OK;
}

static void slicer_stop(void)
{
    sl.playing = false;
    free(sl.buf);
    sl.buf = NULL;
}

// ---- audio ---------------------------------------------------------------
static void slicer_process(int32_t out[MACHINE_BLOCK],
                           const int32_t in[MACHINE_BLOCK],
                           const machine_io_t *io)
{
    (void)in;
    if (sl.loading || sl.len == 0 || !sl.buf) { memset(out, 0, MACHINE_BLOCK * sizeof(int32_t)); return; }

    // TR buttons (active low): TR1 = fire selected slice, TR2 = fire + step
    static uint8_t prev_trig = 0x03;
    uint8_t pressed = prev_trig & (~io->trig_level) & 0x03;
    prev_trig = io->trig_level;
    if (pressed & 1) sl.cmd_fire = 1;
    if (pressed & 2) sl.cmd_advance = 1;

    // knob 6 (CV6) selects the slice — update on knob movement so the encoder
    // still works when the knob is still (both write sl.sel, last mover wins)
    static uint16_t last_cv6 = 0xFFFF;
    uint16_t cv6 = io->cv[5];
    if (last_cv6 == 0xFFFF) last_cv6 = cv6;
    if (cv6 > last_cv6 + 40 || cv6 + 40 < last_cv6) {
        int s = (int)((uint32_t)cv6 * sl.n_slices / 4096);
        sl.sel = (s >= sl.n_slices) ? sl.n_slices - 1 : s;
        last_cv6 = cv6;
    }

    // CV1 jack = level (when driven, else unity — unpatched plays full volume)
    uint16_t c1 = io->cv[0] > 900 ? io->cv[0] - 900 : 0;   // 0..3195
    sl.level = c1 ? (uint16_t)((uint32_t)c1 * 255 / 3195) : 255;

    // knob 7 (CV7) pitch: a unity plateau around center (easy to hit 100%),
    // scaling to 0.5x .. 2.0x outside it
    sl.pitch_cv = io->cv[6];
    if (sl.pitch_cv >= 1843 && sl.pitch_cv <= 2253) sl.inc = 1.0f;         // ~±10% unity
    else if (sl.pitch_cv > 2253) sl.inc = 1.0f + (float)(sl.pitch_cv - 2253) / 1842.0f;
    else                         sl.inc = 0.5f + (float)sl.pitch_cv / 1843.0f * 0.5f;

    if (sl.cmd_fire)    { sl.cmd_fire = 0;    fire_slice(sl.sel); }
    if (sl.cmd_advance) { sl.cmd_advance = 0; fire_slice(sl.sel); sl.sel = (sl.sel + 1) % sl.n_slices; }
    if (sl.auto_on && !sl.playing) fire_slice(sl.cur);       // keep the auto sequence running

    int frames = MACHINE_BLOCK / 2;
    for (int f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        if (sl.playing) {
            uint32_t i0 = (uint32_t)sl.pos;
            uint32_t i1 = i0 + 1;
            if (i1 >= sl.len) i1 = i0;
            float frac = (float)(sl.pos - (double)i0);
            int l0, l1, r0, r1;
            if (sl.mono) { l0 = r0 = sl.buf[i0]; l1 = r1 = sl.buf[i1]; }
            else { l0 = sl.buf[i0 * 2]; l1 = sl.buf[i1 * 2];
                   r0 = sl.buf[i0 * 2 + 1]; r1 = sl.buf[i1 * 2 + 1]; }
            l = (l0 + (int)((l1 - l0) * frac)) * sl.level >> 8;
            r = (r0 + (int)((r1 - r0) * frac)) * sl.level >> 8;

            if (sl.reverse) {
                sl.pos -= sl.inc;
                if (sl.pos < (double)sl.s_start) end_slice();
            } else {
                sl.pos += sl.inc;
                if (sl.pos >= (double)sl.s_end) end_slice();
            }
        }
        out[f * 2]     = l << 16;
        out[f * 2 + 1] = r << 16;
    }
}

// ---- sample I/O (UI task) -------------------------------------------------
static void compute_peaks(void)
{
    for (int c = 0; c < SL_PEAKS; c++) {
        uint32_t a = (uint32_t)((uint64_t)c * sl.len / SL_PEAKS);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * sl.len / SL_PEAKS);
        int peak = 0;
        for (uint32_t i = a; i < b; i++) {
            int v = sl.mono ? sl.buf[i] : sl.buf[i * 2];     if (v < 0) v = -v;
            int w = sl.mono ? v         : sl.buf[i * 2 + 1]; if (w < 0) w = -w;
            if (v > peak) peak = v;
            if (w > peak) peak = w;
        }
        sl.peaks[c] = (uint8_t)((peak * 31) / 32768);
    }
    sl.peak_n = (sl.len > 0) ? SL_PEAKS : 0;
}

int slicer_load(const char *name)
{
    if (!sl.buf) return -1;
    sl.loading = true;
    sl.playing = false;
    // mono load packs one int16 per frame — same slab, double the seconds
    sl.mono = sl.load_mono;
    uint32_t cap = sl.mono ? sl.cap_frames * 2 : sl.cap_frames;
    uint32_t n = sample_load(name, sl.buf, cap, sl.mono);
    if (n == 0) { sl.loading = false; return -1; }
    sl.len = n;
    strncpy(sl.sample, name, sizeof(sl.sample) - 1);
    sl.sample[sizeof(sl.sample) - 1] = 0;
    compute_peaks();
    compute_envelope();     // onset envelope for transient detection
    // Octatrack sidecar: if usr/<name>.OT exists its chops auto-apply
    int otn = slicer_parse_ot(name, sl.len, sl.ot_pt, SL_OT_SLICES + 1);
    sl.ot_n = otn > 0 ? otn : 0;
    sl.ot_present = sl.ot_active = (otn > 0);
    recompute_slices();     // (re)build slice boundaries for the new sample
    sl.cur = 0;
    sl.sel = 0;
    sl.loading = false;
    ESP_LOGI("SLICER", "loaded %s (%lu frames, %d slices)", name, (unsigned long)n, sl.n_slices);
    return 0;
}

int slicer_list_samples(char out[][24], int max)
{
    return sample_list(out, max);
}

// persist settings + the loaded sample name so the slicer comes back the way
// you left it (reloads the remembered sample on bind)
static cJSON *slicer_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "slices", sl.slice_target);
    cJSON_AddBoolToObject(o, "transient", sl.transient_mode);
    cJSON_AddNumberToObject(o, "sens", sl.sensitivity);
    cJSON_AddStringToObject(o, "sample", sl.sample);
    cJSON_AddBoolToObject(o, "auto", sl.auto_on);
    cJSON_AddBoolToObject(o, "reverse", sl.reverse);
    cJSON_AddBoolToObject(o, "mono", sl.load_mono);
    return o;
}

static void slicer_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "slices")) && cJSON_IsNumber(j)) sl.slice_target = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "transient"))) sl.transient_mode = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sens")) && cJSON_IsNumber(j)) sl.sensitivity = j->valueint;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "auto")))    sl.auto_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "reverse"))) sl.reverse = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "mono"))) sl.load_mono = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "sample")) && cJSON_IsString(j) && j->valuestring[0])
        slicer_load(j->valuestring);   // reload the remembered sample (rebuilds slices)
}

extern const machine_ui_t slicer_menu_ui;

const machine_t machine_slicer = {
    .name = "Slicer",
    .start = slicer_start,
    .stop = slicer_stop,
    .process = slicer_process,
    .preset_save = slicer_preset_save,
    .preset_load = slicer_preset_load,
    .ui = &slicer_menu_ui,
};
