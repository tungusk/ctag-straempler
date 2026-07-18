// Tape engine (see tape_priv.h). process() reads/writes the PSRAM tape only —
// destructive edits (cut/paste/normalize/...) are UI-context and REQUIRE the
// transport stopped, so the audio task never sees a moving buffer.
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "machine.h"
#include "cvsmooth.h"
#include "sample_ram.h"
#include "sampfile.h"
#include "sd_lock.h"
#include "fxchain.h"
#include "tape_priv.h"

static const char *TAG = "TAPE";
tape_state_t tp;

static const uint32_t TP_LEN_SECS[TP_LEN_OPTS] = { 15, 30, 60 };

// ---- bank alloc (1 MiB blocks, fail-soft) ---------------------------------------
static void bank_free(tp_bank_t *b)
{
    for (int i = 0; i < b->nblk; i++)
        if (b->blk[i]) { heap_caps_free(b->blk[i]); b->blk[i] = NULL; }
    b->nblk = 0; b->cap = 0;
}

// allocate banks to cover `frames`; trims to what the heap gives (fail-soft).
static int bank_alloc(tp_bank_t *b, uint32_t frames)
{
    bank_free(b);
    int want = (int)((frames + TP_BLK_FRAMES - 1) / TP_BLK_FRAMES);
    if (want > TP_MAX_BLK) want = TP_MAX_BLK;
    for (int i = 0; i < want; i++) {
        b->blk[i] = heap_caps_malloc(TP_BLK_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!b->blk[i]) break;
        b->nblk = i + 1;
    }
    if (b->nblk == 0) return -1;
    uint32_t got = (uint32_t)b->nblk * TP_BLK_FRAMES;
    b->cap = got < frames ? got : frames;
    return b->cap == frames ? 0 : 1;      // 1 = trimmed
}

// ---- helpers -----------------------------------------------------------------
uint32_t tape_beat_frames(void)
{
    float bpm = clockin_beat_bpm(&tp.ci);
    bool clk = bpm > 0;
    if (!clk) bpm = tp.manual_bpm;
    tp.disp_bpm = bpm;
    tp.disp_clk = clk;
    if (bpm < 20.0f) bpm = 20.0f;
    return (uint32_t)((float)TP_RATE * 60.0f / bpm);
}

void tape_eff_window(uint32_t *in, uint32_t *out)
{
    long i = (long)tp.in_pt, o = (long)tp.out_pt;
    if (tp.len == 0) { *in = *out = 0; return; }
    long mov = (long)(tp.win_move * (float)tp.len);
    i += mov; o += mov;
    long w = o - i;
    if (w < 64) w = 64;
    if (i < 0) i = 0;
    if (i > (long)tp.len - 64) i = (long)tp.len - 64;
    o = i + w;
    if (o > (long)tp.len) o = (long)tp.len;
    if (o <= i) o = i + 1;
    *in = (uint32_t)i; *out = (uint32_t)o;
}

uint32_t tape_snap(uint32_t frame)
{
    if (tp.len == 0) return 0;
    if (frame > tp.len) frame = tp.len;
    if (tp.disp_bpm > 0 || clockin_beat_bpm(&tp.ci) > 0) {   // grid snap, IN = beat 0
        uint32_t b = tape_beat_frames();
        long rel = (long)frame - (long)tp.in_pt;
        long snapped = (long)tp.in_pt + ((rel + (long)b / 2) / (long)b) * (long)b;
        if (snapped < 0) snapped = 0;
        if (snapped > (long)tp.len) snapped = tp.len;
        return (uint32_t)snapped;
    }
    // no grid: nearest rising zero-cross within +/-1024
    for (int r = 0; r < 1024; r++)
        for (int s = -1; s <= 1; s += 2) {
            long i = (long)frame + (long)s * r;
            if (i < 1 || i >= (long)tp.len) continue;
            if (tp_rd((uint32_t)i - 1) <= 0 && tp_rd((uint32_t)i) > 0) return (uint32_t)i;
        }
    return frame;
}

static bool tp_stopped(void) { return !tp.playing && !tp.recording; }

// ---- transport + dsp -----------------------------------------------------------
static inline float tp_softclip(float x, float amt)
{
    // cubic soft clip, drive blends dry->driven (the drums' recipe)
    float g = 1.0f + amt * 3.0f;
    float y = x * g;
    if (y > 1.0f) y = 1.0f; else if (y < -1.0f) y = -1.0f;
    y = y - (y * y * y) / 3.0f;                 // max |y| = 2/3
    return y * 1.5f;
}

// start recording: roll from the crop IN (or 0 on an empty tape), then arm
static void tape_rec_start(void)
{
    if (!tp.playing) {
        uint32_t ein, eout; tape_eff_window(&ein, &eout);
        tp.pos = tp.len ? (double)ein : 0.0;
        tp.playing = true;
    }
    tp.recording = true;
}

static void tape_process(int32_t out[MACHINE_BLOCK],
                         const int32_t in[MACHINE_BLOCK],
                         const machine_io_t *io)
{
    static cvmed_t med[8];
    int cvm[8];
    for (int k = 0; k < 8; k++) cvm[k] = cvmed_step(&med[k], io->cv[k]);

    clockin_block(&tp.ci, clock_source_level(tp.clk_src, io), MACHINE_BLOCK / 2);

    // knobs 5..8 takeover: win move / cutoff / res / drive
    float kn[4] = { (float)cvm[4]/4095.0f, (float)cvm[5]/4095.0f,
                    (float)cvm[6]/4095.0f, (float)cvm[7]/4095.0f };
    if (tp.knob_ctx != 0) {
        tp.knob_ctx = 0;
        for (int i = 0; i < 4; i++) { tp.knob_capt[i] = kn[i]; tp.knob_live[i] = false; }
    }
    for (int i = 0; i < 4; i++)
        if (!tp.knob_live[i] && fabsf(kn[i] - tp.knob_capt[i]) > 0.03f) tp.knob_live[i] = true;
    if (tp.knob_live[0]) tp.win_move = (kn[0] - 0.5f) * 2.0f;              // K5 noon = home
    if (tp.knob_live[1]) tp.cutoff   = 30.0f * powf(200.0f, kn[1]);        // 30 Hz .. 6 kHz
    if (tp.knob_live[2]) tp.res01    = kn[2];
    if (tp.knob_live[3]) tp.drive    = kn[3];

    // transport edges: TR1 play/stop, TR2 record punch
    if (io->trig_rising & 1) {
        if (tp.playing) { tp.playing = false; tp.recording = false; }
        else if (tp.len || tp.rec_src == TPS_INPUT) {
            uint32_t ein, eout; tape_eff_window(&ein, &eout);
            tp.pos = (double)ein;
            tp.playing = true;
        }
    }
    // TR2 record. PUNCH: each validated gate edge toggles record in/out.
    // MOMENTARY: record only while the gate is HELD (active low -> bit clear).
    if (tp.rec_mode == TPR_MOMENTARY) {
        bool gate = !(io->trig_level & 2);
        if (gate && !tp.recording)      tape_rec_start();
        else if (!gate && tp.recording) tp.recording = false;
    } else {
        if (io->trig_rising & 2) {
            if (tp.recording) tp.recording = false;
            else              tape_rec_start();
        }
    }

    uint32_t ein = 0, eout = 0;
    tape_eff_window(&ein, &eout);
    bool empty_rec = (tp.len == 0);                  // first recording fills from 0

    float coef = 0, q = 0;
    if (tp.flt_mode != TPF_OFF) {
        float fc = tp_clampf(tp.cutoff, 20.0f, 6500.0f);
        coef = svf_coef(fc, TP_RATE, 1.0f);
        q = svf_damp(tp.res01, 0.6f, 2.0f);
        if (!(fabsf(tp.flt.lp) < 1e9f) || !(fabsf(tp.flt.bp) < 1e9f)) svf_reset(&tp.flt);
    }

    bool have_tape = tp.tape.nblk > 0;
    int frames = MACHINE_BLOCK / 2;
    for (int f = 0; f < frames; f++) {
        // source: input while recording-from-input or monitoring; else tape
        float src = 0.0f;
        uint32_t p = (uint32_t)tp.pos;
        bool on_tape = tp.playing && have_tape && tp.len && p < tp.len;
        float in_mid = (float)(((int32_t)(int16_t)(in[f*2] >> 16) +
                                (int32_t)(int16_t)(in[f*2+1] >> 16)) >> 1) / 32768.0f;
        if (tp.recording && tp.rec_src == TPS_INPUT)      src = in_mid;
        else if (on_tape)                                 src = (float)tp_rd(p) / 32768.0f;
        else if (!tp.playing && tp.monitor)               src = in_mid;

        // fx chain: filter -> drive (reverb runs on the block below)
        float y = src;
        if (tp.flt_mode != TPF_OFF) {
            float lp, bp, hp;
            svf_step(&tp.flt, y, coef, q, &lp, &bp, &hp);
            y = tp.flt_mode == TPF_LP ? lp : tp.flt_mode == TPF_BP ? bp : hp;
        }
        if (tp.drive > 0.005f) y = tp_softclip(y, tp.drive);

        // record head taps POST-FX (print the chain); recording extends len
        if (tp.recording && have_tape) {
            uint32_t w = (uint32_t)tp.pos;
            if (empty_rec && w < tp.cap) {
                float v = y * 32767.0f;
                tp_wr(w, (int16_t)tp_clampf(v, -32768.0f, 32767.0f));
                if (w + 1 > tp.len) tp.len = w + 1;
                if (w + 1 >= tp.cap) { tp.recording = false; tp.playing = false; }
            } else if (!empty_rec && w < tp.len) {
                float v = y * 32767.0f;
                tp_wr(w, (int16_t)tp_clampf(v, -32768.0f, 32767.0f));
            }
        }

        // advance + loop the crop window (or the whole fill while first-recording)
        if (tp.playing) {
            tp.pos += 1.0;
            if (!empty_rec) {
                if (tp.pos >= (double)eout || tp.pos >= (double)tp.len)
                    tp.pos = (double)ein;
            }
        }

        float o = y * tp.level * 28000.0f;
        if (o > 32000.0f) o = 32000.0f; else if (o < -32000.0f) o = -32000.0f;
        int32_t s = ((int32_t)(int16_t)o) << 16;
        out[f * 2] = s;
        out[f * 2 + 1] = s;
    }

    // FX chain: overdrive -> flanger -> tremolo -> delay -> reverb. The rated
    // effects are clock-synced — the current grid BPM sets their rate/tap so
    // they lock to tempo (bpm computed once and reused across the block).
    float bpm = clockin_beat_bpm(&tp.ci);
    if (bpm <= 0.0f) bpm = tp.manual_bpm;
    // Run the chain in float with a single soft limiter at the end (fxchain.h)
    // so stacked effects don't hard-clip at every stage.
    {
        float fb[FX_SCRATCH_N];
        fx_unpack_i32(out, fb, frames * 2);
        if (tp.od_on) overdrive_block_f(&tp.od, fb, frames);
        if (tp.flg_on && tp.flg.bufL) {
            flanger_set_rate_beats(&tp.flg, tp_dly_beats[tp.flg_div], bpm);
            flanger_block_f(&tp.flg, fb, frames);
        }
        if (tp.trem_on) {
            tremolo_set_rate_beats(&tp.trem, tp_dly_beats[tp.trem_div], bpm);
            tremolo_block_f(&tp.trem, fb, frames);
        }
        if (tp.dly_on && tp.dly.bufL) {
            fxdelay_set_time_beats(&tp.dly, tp_dly_beats[tp.dly_div], bpm);
            fxdelay_block_f(&tp.dly, fb, frames);
        }
        if (tp.rv.mode != RV_OFF && tp.rv.slab) {
            reverb_block_f(&tp.rv, fb, frames);
            // reverb tail is output-only; the record head already wrote pre-reverb
            // this block. Printing reverb: set Rec Source = TAPE and punch a pass.
        }
        fx_pack_softclip(fb, out, frames * 2);
    }
}

// ---- edits (UI context, transport stopped) -------------------------------------
static void crop_clamp(void)
{
    if (tp.len == 0) { tp.in_pt = tp.out_pt = 0; return; }
    if (tp.out_pt > tp.len) tp.out_pt = tp.len;
    if (tp.in_pt >= tp.out_pt) tp.in_pt = tp.out_pt > 64 ? tp.out_pt - 64 : 0;
}

int tape_set_len_sel(int sel)
{
    if (!tp_stopped()) return -1;
    sel = tp_clampi(sel, 0, TP_LEN_OPTS - 1);
    uint32_t want = TP_LEN_SECS[sel] * TP_RATE;
    if (bank_alloc(&tp.tape, want) < 0) { tp.cap = 0; return -2; }
    tp.cap = tp.tape.cap;                       // may be trimmed (fail-soft)
    tp.len_sel = sel;
    tp.len = 0; tp.in_pt = tp.out_pt = 0; tp.pos = 0;
    memset(tp.peaks, 0, sizeof(tp.peaks));
    tp.peaks_done = 0;
    return 0;
}

int tape_load(const char *name)
{
    if (!tp_stopped() || tp.tape.nblk == 0 || !name || !name[0]) return -1;
    char path[64];
    if (sample_resolve(name, path, sizeof(path)) != 0) return -2;

    // stream through sampfile in bursts; staging is INTERNAL DMA RAM (FatFS
    // can hand the buffer straight to SDMMC, which cannot target PSRAM)
    const int BURST = 1024;
    int16_t *stage = heap_caps_malloc((size_t)BURST * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    if (!stage) return -3;

    sd_lock_take();
    FILE *f = fopen(path, "rb");
    sampfile_t sf;
    bool ok = f && sampfile_probe(f, &sf) == 0;
    sd_lock_give();
    if (!ok) { if (f) { sd_lock_take(); fclose(f); sd_lock_give(); } heap_caps_free(stage); return -4; }

    uint32_t total = sf.frames;
    if (total > tp.cap) total = tp.cap;
    uint32_t done = 0;
    while (done < total) {
        int n = (int)(total - done > (uint32_t)BURST ? (uint32_t)BURST : total - done);
        sd_lock_take();
        size_t got = sampfile_read(f, &sf, stage, (size_t)n);
        sd_lock_give();
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            int32_t mid = ((int32_t)stage[i*2] + (int32_t)stage[i*2+1]) >> 1;
            tp_wr(done + (uint32_t)i, (int16_t)mid);
        }
        done += (uint32_t)got;
        if ((done & 0x3FFFF) == 0) vTaskDelay(1);      // breathe every ~256k frames
    }
    sd_lock_take();
    fclose(f);
    sd_lock_give();
    heap_caps_free(stage);
    if (done < 2) return -5;

    tp.len = done;
    tp.in_pt = 0; tp.out_pt = done; tp.pos = 0;
    tape_rebuild_peaks(true);
    return 0;
}

void tape_clear(void)
{
    if (!tp_stopped()) return;
    tp.len = 0; tp.in_pt = tp.out_pt = 0; tp.pos = 0;
    memset(tp.peaks, 0, sizeof(tp.peaks));
    tp.peaks_done = 0;
}

void tape_norm(void)
{
    if (!tp_stopped() || tp.len == 0) return;
    crop_clamp();
    int pk = 1;
    for (uint32_t i = tp.in_pt; i < tp.out_pt; i++) { int v = tp_rd(i); if (v < 0) v = -v; if (v > pk) pk = v; }
    float g = 31000.0f / (float)pk;
    if (g <= 1.001f && g >= 0.999f) return;
    for (uint32_t i = tp.in_pt; i < tp.out_pt; i++) {
        float v = (float)tp_rd(i) * g;
        tp_wr(i, (int16_t)tp_clampf(v, -32768.0f, 32767.0f));
    }
    tape_rebuild_peaks(true);
}

void tape_reverse(void)
{
    if (!tp_stopped() || tp.len == 0) return;
    crop_clamp();
    uint32_t a = tp.in_pt, b = tp.out_pt - 1;
    while (a < b) { int16_t t = tp_rd(a); tp_wr(a, tp_rd(b)); tp_wr(b, t); a++; b--; }
    tape_rebuild_peaks(true);
}

void tape_fade(void)
{
    if (!tp_stopped() || tp.len == 0) return;
    crop_clamp();
    uint32_t n = tp.out_pt - tp.in_pt;
    uint32_t F = TP_RATE * TP_FADE_MS / 1000;
    if (F * 2 > n) F = n / 2;
    for (uint32_t i = 0; i < F; i++) {
        float g = (float)i / (float)F;
        tp_wr(tp.in_pt + i, (int16_t)((float)tp_rd(tp.in_pt + i) * g));
        tp_wr(tp.out_pt - 1 - i, (int16_t)((float)tp_rd(tp.out_pt - 1 - i) * g));
    }
    tape_rebuild_peaks(true);
}

int tape_copy(void)
{
    if (!tp_stopped() || tp.len == 0) return -1;
    crop_clamp();
    uint32_t n = tp.out_pt - tp.in_pt;
    if (bank_alloc(&tp.clip, n) < 0) { tp.clip_len = 0; return -2; }
    if (tp.clip.cap < n) n = tp.clip.cap;             // trimmed (fail-soft)
    for (uint32_t i = 0; i < n; i++) bank_wr(&tp.clip, i, tp_rd(tp.in_pt + i));
    tp.clip_len = n;
    return 0;
}

int tape_cut(void)
{
    if (tape_copy() != 0) return -1;
    uint32_t n = tp.out_pt - tp.in_pt;
    for (uint32_t i = tp.out_pt; i < tp.len; i++)     // close the gap (forward)
        tp_wr(i - n, tp_rd(i));
    tp.len -= n;
    tp.out_pt = tp.in_pt;                             // collapse: splice point
    if (tp.len) {
        if (tp.out_pt >= tp.len) tp.out_pt = tp.len;
        if (tp.out_pt == tp.in_pt) tp.out_pt = tp.in_pt + 64 <= tp.len ? tp.in_pt + 64 : tp.len;
        crop_clamp();
    } else tp.in_pt = tp.out_pt = 0;
    if (tp.pos > (double)tp.len) tp.pos = 0;
    tape_rebuild_peaks(true);
    return 0;
}

int tape_paste(void)
{
    if (!tp_stopped() || tp.clip_len == 0 || tp.tape.nblk == 0) return -1;
    uint32_t n = tp.clip_len;
    if (tp.len + n > tp.cap) n = tp.cap - tp.len;     // clamp: paste what fits
    if (n == 0) return -2;
    uint32_t at = tp.len ? tp.in_pt : 0;
    for (uint32_t i = tp.len; i > at; i--)            // shift right (backward)
        tp_wr(i - 1 + n, tp_rd(i - 1));
    for (uint32_t i = 0; i < n; i++)
        tp_wr(at + i, bank_rd(&tp.clip, i));
    tp.len += n;
    tp.in_pt = at;
    tp.out_pt = at + n;                               // crop = the pasted material
    tape_rebuild_peaks(true);
    return 0;
}

void tape_crop_beats(int beats)
{
    if (tp.len == 0) return;
    uint32_t b = tape_beat_frames();
    uint64_t o = (uint64_t)tp.in_pt + (uint64_t)b * (uint32_t)beats;
    tp.out_pt = o > tp.len ? tp.len : (uint32_t)o;
    crop_clamp();
}

// ---- save crop -> pool take (background job) -----------------------------------
static void save_task(void *pv)
{
    (void)pv;
    uint32_t a = tp.in_pt, b = tp.out_pt;
    char path[48];
    snprintf(path, sizeof(path), "/sdcard/usr/%s.WAV", tp.save_id);
    // chunk on the HEAP, not the task stack (first hw run wedged in
    // "saving...": 2 KB stack buffer + FatFS frames overflowed 4 KB)
    int16_t *chunk = heap_caps_malloc(512 * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
    FILE *f = NULL;
    sd_lock_take();
    f = fopen(path, "wb");
    if (f) sampwav_start(f);
    sd_lock_give();
    if (!f || !chunk) {
        if (f) { sd_lock_take(); fclose(f); sd_lock_give(); }
        if (chunk) heap_caps_free(chunk);
        ESP_LOGE(TAG, "save: %s failed", f ? "alloc" : "fopen");
        tp.save_id[0] = 0; tp.save_busy = false;
        vTaskDelete(NULL); return;
    }

    for (uint32_t i = a; i < b; ) {
        int n = 0;
        while (n < 512 && i < b) { int16_t v = tp_rd(i); chunk[n*2] = v; chunk[n*2+1] = v; n++; i++; }
        sd_lock_take();
        fwrite(chunk, sizeof(int16_t) * 2, n, f);
        sd_lock_give();
        if ((i & 0xFFFF) < 1024) vTaskDelay(1);   // yield ~every 64k frames, not every chunk
    }
    sd_lock_take();
    sampwav_finish(f);
    fclose(f);
    // minimal sidecar: /files lists ONLY ids with a .JSN ("complete sample")
    char jp[48];
    snprintf(jp, sizeof(jp), "/sdcard/usr/%s.JSN", tp.save_id);
    FILE *jf = fopen(jp, "w");
    if (jf) { fputs("{\"src\":\"tape\"}", jf); fclose(jf); }
    sd_lock_give();
    heap_caps_free(chunk);
    ESP_LOGI(TAG, "saved crop -> %s", tp.save_id);
    tp.save_busy = false;
    vTaskDelete(NULL);
}

int tape_save_crop(void)
{
    if (!tp_stopped() || tp.len == 0 || tp.save_busy) return -1;
    crop_clamp();
    int idx = sample_next_index("TAP_");
    if (idx < 0) idx = 0;
    if (idx > 9999) idx = 9999;                // 8.3: id stays exactly 8 chars
    snprintf(tp.save_id, sizeof(tp.save_id), "TAP_%04d", idx % 10000);
    tp.save_busy = true;
    if (xTaskCreate(save_task, "tape_sv", 8192, NULL, 4, NULL) != pdPASS) {
        tp.save_busy = false; tp.save_id[0] = 0; return -2;
    }
    return 0;
}

// ---- peaks (UI task) ------------------------------------------------------------
void tape_rebuild_peaks(bool full)
{
    if (tp.tape.nblk == 0 || tp.cap == 0) return;
    uint32_t upto = tp.len;
    uint32_t from = full ? 0 : tp.peaks_done;
    if (full) memset(tp.peaks, 0, sizeof(tp.peaks));
    if (upto == 0) { tp.peaks_done = 0; return; }
    // columns span the WHOLE tape cap, so material sits where it sits
    int c0 = (int)((uint64_t)from * TP_PEAKS / tp.cap);
    int c1 = (int)((uint64_t)(upto - 1) * TP_PEAKS / tp.cap);
    for (int c = c0; c <= c1 && c < TP_PEAKS; c++) {
        uint32_t a = (uint32_t)((uint64_t)c * tp.cap / TP_PEAKS);
        uint32_t b = (uint32_t)((uint64_t)(c + 1) * tp.cap / TP_PEAKS);
        if (b > upto) b = upto;
        if (a >= b) continue;
        uint32_t step = (b - a) / 64 + 1;
        int pk = 0;
        for (uint32_t s = a; s < b; s += step) { int v = tp_rd(s); if (v < 0) v = -v; if (v > pk) pk = v; }
        pk >>= 7;
        tp.peaks[c] = (uint8_t)(pk > 255 ? 255 : pk);
    }
    tp.peaks_done = upto;
}

// ---- lifecycle / preset ----------------------------------------------------------
// clock-synced delay divisions — see tape_priv.h (beat = quarter note)
const float       tp_dly_beats[TP_DLY_NDIV] = { 0.25f, 1.0f/3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
const char *const tp_dly_names[TP_DLY_NDIV] = { "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2" };

static esp_err_t tape_start(void)
{
    memset(&tp, 0, sizeof(tp));
    tp.len_sel = 1;                            // 30 s default
    tp.manual_bpm = 120.0f;
    tp.dly_div = 2;                            // 1/8 note default when delay is enabled
    tp.trem_div = 2; tp.flg_div = 4;           // musical-division defaults
    tp.clk_src = clock_source_clamp_cv_audio(7);
    tp.level = 0.9f;
    tp.cutoff = 2000.0f;
    tp.res01 = 0.1f;
    tp.flt_mode = TPF_OFF;
    tp.monitor = true;
    tp.knob_ctx = -1;
    clockin_reset(&tp.ci, 1.0f);
    svf_reset(&tp.flt);
    if (bank_alloc(&tp.tape, TP_LEN_SECS[tp.len_sel] * TP_RATE) < 0) {
        ESP_LOGE(TAG, "tape bank alloc failed");
        return ESP_ERR_NO_MEM;
    }
    tp.cap = tp.tape.cap;
    return ESP_OK;
}

static void tape_stop(void)
{
    tp.playing = false; tp.recording = false;
    while (tp.save_busy) vTaskDelay(pdMS_TO_TICKS(20));   // job reads the tape
    bank_free(&tp.tape);
    bank_free(&tp.clip);
    tp.clip_len = 0;
    tp.cap = 0; tp.len = 0;
    reverb_free(&tp.rv);
    fxdelay_free(&tp.dly);
    flanger_free(&tp.flg);
}

static cJSON *tape_preset_save(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "lsel", tp.len_sel);
    cJSON_AddNumberToObject(o, "clk", tp.clk_src);
    cJSON_AddNumberToObject(o, "mbpm", tp.manual_bpm);
    cJSON_AddNumberToObject(o, "flt", tp.flt_mode);
    cJSON_AddNumberToObject(o, "cut", tp.cutoff);
    cJSON_AddNumberToObject(o, "res", tp.res01);
    cJSON_AddNumberToObject(o, "drv", tp.drive);
    cJSON_AddNumberToObject(o, "rv", tp.rv.mode);
    cJSON_AddNumberToObject(o, "rvmx", (int)(tp.rv.wet * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "dly", tp.dly_on);
    cJSON_AddNumberToObject(o, "dlydv", tp.dly_div);
    cJSON_AddNumberToObject(o, "dlyfb", (int)(tp.dly.fb * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "dlymx", (int)(tp.dly.wet * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "dlytn", (int)(tp.dly.damp * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "dlypp", tp.dly.pingpong);
    cJSON_AddBoolToObject(o, "od", tp.od_on);
    cJSON_AddNumberToObject(o, "oddr", (int)(tp.od.drive * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "odtn", (int)(tp.od.tone * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "odbs", (int)(tp.od.bias * 100 + (tp.od.bias < 0 ? -0.5f : 0.5f)));
    cJSON_AddNumberToObject(o, "odlv", (int)(tp.od.level * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "flg", tp.flg_on);
    cJSON_AddNumberToObject(o, "flgdv", tp.flg_div);
    cJSON_AddNumberToObject(o, "flgdp", (int)(tp.flg.depth * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "flgfb", (int)(tp.flg.fb * 100 + (tp.flg.fb < 0 ? -0.5f : 0.5f)));
    cJSON_AddNumberToObject(o, "flgmx", (int)(tp.flg.wet * 100 + 0.5f));
    cJSON_AddBoolToObject(o, "trem", tp.trem_on);
    cJSON_AddNumberToObject(o, "trmdv", tp.trem_div);
    cJSON_AddNumberToObject(o, "trmdp", (int)(tp.trem.depth * 100 + 0.5f));
    cJSON_AddNumberToObject(o, "trmsh", tp.trem.shape);
    cJSON_AddBoolToObject(o, "trmst", tp.trem.stereo);
    cJSON_AddNumberToObject(o, "lvl", tp.level);
    cJSON_AddNumberToObject(o, "rsrc", tp.rec_src);
    cJSON_AddNumberToObject(o, "rmode", tp.rec_mode);
    cJSON_AddBoolToObject(o, "mon", tp.monitor);
    return o;
}

static void tape_preset_load(const cJSON *node)
{
    if (!node) return;
    cJSON *j;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lsel")) && cJSON_IsNumber(j)) {
        int s = tp_clampi(j->valueint, 0, TP_LEN_OPTS - 1);
        if (s != tp.len_sel) tape_set_len_sel(s);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "clk"))  && cJSON_IsNumber(j)) tp.clk_src = clock_source_clamp_cv_audio(j->valueint);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "mbpm")) && cJSON_IsNumber(j)) tp.manual_bpm = tp_clampf((float)j->valuedouble, 40, 240);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "flt"))  && cJSON_IsNumber(j)) tp.flt_mode = tp_clampi(j->valueint, 0, TPF_N - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "cut"))  && cJSON_IsNumber(j)) tp.cutoff = tp_clampf((float)j->valuedouble, 30, 6000);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "res"))  && cJSON_IsNumber(j)) tp.res01 = tp_clampf((float)j->valuedouble, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "drv"))  && cJSON_IsNumber(j)) tp.drive = tp_clampf((float)j->valuedouble, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rv"))   && cJSON_IsNumber(j)) {
        int m = j->valueint; if (m < 0 || m >= RV_N_MODES) m = RV_OFF;
        if (m != RV_OFF && !tp.rv.slab && reverb_init(&tp.rv) != ESP_OK) m = RV_OFF;
        reverb_set_mode(&tp.rv, m);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "dly"))   && cJSON_IsBool(j)) {
        bool on = cJSON_IsTrue(j);
        if (on && !tp.dly.bufL && fxdelay_init(&tp.dly) != ESP_OK) on = false;
        tp.dly_on = on;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlydv")) && cJSON_IsNumber(j)) tp.dly_div = tp_clampi(j->valueint, 0, TP_DLY_NDIV - 1);
    if (tp.dly.bufL) {   // params apply once the slab exists (order-independent)
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlyfb")) && cJSON_IsNumber(j)) fxdelay_set_feedback(&tp.dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlymx")) && cJSON_IsNumber(j)) fxdelay_set_mix(&tp.dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlytn")) && cJSON_IsNumber(j)) fxdelay_set_damp(&tp.dly, (float)j->valueint / 100.0f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "dlypp")) && cJSON_IsBool(j))   fxdelay_set_pingpong(&tp.dly, cJSON_IsTrue(j));
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "od"))   && cJSON_IsBool(j))   tp.od_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "oddr")) && cJSON_IsNumber(j)) tp.od.drive = tp_clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odtn")) && cJSON_IsNumber(j)) tp.od.tone  = tp_clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odbs")) && cJSON_IsNumber(j)) tp.od.bias  = tp_clampf((float)j->valueint / 100.0f, -1, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "odlv")) && cJSON_IsNumber(j)) tp.od.level = tp_clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "flg"))  && cJSON_IsBool(j)) {
        bool on = cJSON_IsTrue(j);
        if (on && !tp.flg.bufL && flanger_init(&tp.flg) != ESP_OK) on = false;
        tp.flg_on = on;
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgdv")) && cJSON_IsNumber(j)) tp.flg_div = tp_clampi(j->valueint, 0, TP_DLY_NDIV - 1);
    if (tp.flg.bufL) {   // params apply once the slab exists (order-independent)
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgdp")) && cJSON_IsNumber(j)) tp.flg.depth = tp_clampf((float)j->valueint / 100.0f, 0, 1);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgfb")) && cJSON_IsNumber(j)) tp.flg.fb    = tp_clampf((float)j->valueint / 100.0f, -0.95f, 0.95f);
        if ((j = cJSON_GetObjectItemCaseSensitive(node, "flgmx")) && cJSON_IsNumber(j)) tp.flg.wet   = tp_clampf((float)j->valueint / 100.0f, 0, 1);
    }
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trem")) && cJSON_IsBool(j))   tp.trem_on = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmdv")) && cJSON_IsNumber(j)) tp.trem_div = tp_clampi(j->valueint, 0, TP_DLY_NDIV - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmdp")) && cJSON_IsNumber(j)) tp.trem.depth = tp_clampf((float)j->valueint / 100.0f, 0, 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmsh")) && cJSON_IsNumber(j)) tp.trem.shape = tp_clampi(j->valueint, 0, TREM_NSHAPE - 1);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "trmst")) && cJSON_IsBool(j))   tp.trem.stereo = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rvmx")) && cJSON_IsNumber(j)) reverb_set_mix(&tp.rv, (float)j->valueint / 100.0f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lvl"))  && cJSON_IsNumber(j)) tp.level = tp_clampf((float)j->valuedouble, 0, 1.2f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rsrc")) && cJSON_IsNumber(j)) tp.rec_src = j->valueint == TPS_TAPE ? TPS_TAPE : TPS_INPUT;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rmode")) && cJSON_IsNumber(j)) tp.rec_mode = j->valueint == TPR_MOMENTARY ? TPR_MOMENTARY : TPR_PUNCH;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "mon"))  && cJSON_IsBool(j))   tp.monitor = cJSON_IsTrue(j);
    tp.knob_ctx = -1;
}

extern const machine_ui_t tape_menu_ui;

const machine_t machine_tape = {
    .name = "Tape",
    .start = tape_start,
    .stop = tape_stop,
    .process = tape_process,
    .preset_save = tape_preset_save,
    .preset_load = tape_preset_load,
    .ui = &tape_menu_ui,
};
