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
#include "menu_config.h"
#include "recording.h"
#include "tape_priv.h"

static const char *TAG = "TAPE";
tape_state_t tp;

static const uint32_t TP_LEN_SECS[TP_LEN_OPTS] = { 15, 30, 60 };

static int tape_spawn_save(uint32_t a, uint32_t b, bool crop);   // background take writer (below)
static void tape_stash_and_save(void);                          // request async save of the current take

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
    if (tp.clk_src == CLK_SRC_OFF) {        // un-clocked: no beat grid at all
        tp.disp_bpm = 0.0f; tp.disp_clk = false;
        return 0;
    }
    float bpm; bool clk;
    if (tp.clk_src == CLK_SRC_INT) {        // internal: manual BPM, no external in
        bpm = tp.manual_bpm; clk = false;
    } else {
        bpm = clockin_beat_bpm(&tp.ci);
        clk = bpm > 0;
        if (!clk) bpm = tp.manual_bpm;
    }
    tp.disp_bpm = bpm;
    tp.disp_clk = clk;
    if (bpm < 20.0f) bpm = 20.0f;
    return (uint32_t)((float)TP_RATE * 60.0f / bpm);
}

void tape_eff_window(uint32_t *in, uint32_t *out)
{
    long i = (long)tp.in_pt, o = (long)tp.out_pt;
    if (tp.len == 0) { *in = *out = 0; return; }
    // K5 window shift + the CV matrix offsets: WIN slides both points, IN/OUT
    // move their own point (all fractions of len; the clamps below keep the
    // window sane whatever the modulation does)
    long mov = (long)((tp.win_move + tp.mx_win) * (float)tp.len);
    i += mov + (long)(tp.mx_in  * (float)tp.len);
    o += mov + (long)(tp.mx_out * (float)tp.len);
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

// punch IN. From STOP = a FRESH take: clear the tape and fill from 0 (extend
// len as it rolls). While already PLAYING = overdub over the running loop.
static void tape_rec_start(void)
{
    if (!tp.playing) {
        tp.len = 0; tp.pos = 0.0;
        tp.in_pt = 0; tp.out_pt = 0;
        tp.rec_extend = true;            // first-pass fill
        tp.playing = true;
        tp.cropped = false;              // fresh take: not yet cropped
        tp.restore_id[0] = 0;            // new take is unsaved until it's persisted
        tp.take_num++;                   // -> "REC-###" title
    } else {
        tp.rec_extend = false;           // overdub within the existing loop
    }
    tp.recording = true;
    tp.take_dirty = true;                // unsaved recorded audio now in the buffer
}

// punch OUT. Finalize a fresh take (crop = whole take, loop from the start) and
// keep PLAYING so you immediately hear it back; an overdub just disarms record.
// A finalized take is stable (playing, not being overwritten), so auto-save it
// now — no race, and re-recording/erasing later needs no save-before-overwrite.
static void tape_rec_stop(void)
{
    tp.recording = false;
    if (tp.rec_extend) {
        tp.in_pt = 0;
        tp.out_pt = tp.len;
        tp.pos = 0.0;
        tp.rec_extend = false;
        tape_stash_and_save();           // persist the finished take (async, stable buffer)
        return;
    }
    tp.rec_extend = false;
}

// finalize a fresh take at a specific (beat/bar-quantized) length; loop [0,out).
static void tape_rec_stop_at(uint32_t out_len)
{
    tp.recording = false; tp.rec_extend = false; tp.rec_stop_target = 0;
    if (out_len < 1) out_len = 1;
    if (tp.len < out_len) tp.len = out_len;
    tp.in_pt = 0; tp.out_pt = out_len; tp.pos = 0.0;
    tape_stash_and_save();
}

// user asked to stop recording. A fresh take with quantize on keeps rolling to
// the next beat/bar so the loop is a whole number of beats; else stop now.
static void tape_rec_stop_request(void)
{
    if (!tp.recording) return;
    if (tp.rec_extend && tp.rec_quant != TPQ_OFF && !tp.rec_stop_target) {
        uint32_t beat = tape_beat_frames();
        if (beat >= 64) {                                        // a grid exists
            uint32_t unit = (tp.rec_quant == TPQ_BAR) ? beat * 4 : beat;
            uint32_t pos = (uint32_t)tp.pos;
            uint32_t target = ((pos + unit / 2) / unit) * unit;  // nearest boundary
            if (target < unit) target = unit;                    // at least one unit
            if (pos >= target) tape_rec_stop_at(target);         // already there -> now
            else tp.rec_stop_target = target;                    // keep recording to the beat
            return;
        }
    }
    tape_rec_stop();
}

// audio task: if the buffer holds an unsaved take, stage its bounds and ask the
// UI task to write it out (crop if actively cropped, else the whole take). The
// bank memory stays intact until a fresh take actually records over it, so the
// deferred writer reads valid data even though `len` is about to reset.
static void tape_stash_and_save(void)
{
    if (tp.take_dirty && tp.len > 1 && !tp.save_busy && !tp.autosave_req) {
        tp.save_a = tp.cropped ? tp.in_pt : 0;
        tp.save_b = tp.cropped ? tp.out_pt : tp.len;
        tp.save_crop = tp.cropped;
        tp.autosave_req = true;                  // UI task spawns the writer (reads intact banks)
    }
}

// wipe the tape (no record). A TR2 long-hold erases here for immediate visual
// feedback; the fresh take then rolls when the gate is RELEASED (so you set the
// downbeat on release and don't capture the hold). Bank memory is left as-is so
// a pending auto-save can still read it.
static void tape_erase(void)
{
    tp.playing = false; tp.recording = false;
    tp.len = 0; tp.in_pt = tp.out_pt = 0; tp.pos = 0.0;
    tp.peaks_done = 0;
    tp.take_dirty = false; tp.cropped = false; tp.restore_id[0] = 0;
}

// begin a fresh take from a STOPPED tape. Records immediately (reliable) — the
// outgoing take was already auto-saved when it finalized (punch/stop), so there
// is nothing to save-before-overwrite here and no defer/stall.
static void tape_begin_fresh(void)
{
    tp.peaks_done = 0;
    tape_rec_start();                    // rec_start's fresh branch clears len/crop
}

// ---- card-record mode: long takes stream straight to a WAV (no 30 s cap) ------
// The PSRAM tape is bypassed. Incoming audio runs through the filter/drive + FX
// rack (monitored), and while armed each post-FX block is pushed to the shared
// streaming recorder (usr/REC/TAP_NNNN.WAV). TR2 punches record in/out; the UI
// task (tape_card_service) re-arms the writer after each take. Audio task only
// does trigger/finish/push — all atomic/queue, no SD.
static void tape_card_process(int32_t out[MACHINE_BLOCK],
                              const int32_t in[MACHINE_BLOCK],
                              const machine_io_t *io)
{
    bool rec = recording_is_active();
    if (tp.rec_mode == TPR_MOMENTARY) {
        bool gate = !(io->trig_level & 2);
        if (gate && !rec && recording_is_prepared()) { recording_trigger(); tp.pos = 0.0; }
        else if (!gate && rec) recording_finish();
    } else {
        if (io->trig_rising & 2) {
            if (rec) recording_finish();
            else if (recording_is_prepared()) { recording_trigger(); tp.pos = 0.0; }
        }
    }
    tp.recording = recording_is_active();

    float coef = 0, q = 0;
    if (tp.flt_mode != TPF_OFF) {
        float fc = tp_cut_eff();               // Setup cutoff + CV matrix offset
        coef = svf_coef(fc, TP_RATE, 1.0f);
        q = svf_damp(tp.res01, 0.6f, 2.0f);
        if (!(fabsf(tp.flt.lp) < 1e9f) || !(fabsf(tp.flt.bp) < 1e9f)) svf_reset(&tp.flt);
    }
    int frames = MACHINE_BLOCK / 2;
    for (int f = 0; f < frames; f++) {
        float in_mid = (float)(((int32_t)(int16_t)(in[f*2] >> 16) +
                                (int32_t)(int16_t)(in[f*2+1] >> 16)) >> 1) / 32768.0f;
        float y = in_mid;
        if (tp.flt_mode != TPF_OFF) {
            float lp, bp, hp; svf_step(&tp.flt, y, coef, q, &lp, &bp, &hp);
            y = tp.flt_mode == TPF_LP ? lp : tp.flt_mode == TPF_BP ? bp : hp;
        }
        if (tp.drive > 0.005f) y = tp_softclip(y, tp.drive);
        float u = y * 32767.0f;
        if (u > 32767.0f) u = 32767.0f; else if (u < -32768.0f) u = -32768.0f;
        int32_t su = ((int32_t)(int16_t)u) << 16;
        out[f * 2] = su; out[f * 2 + 1] = su;
    }
    float bpm = clockin_beat_bpm(&tp.ci);
    if (bpm <= 0.0f) bpm = tp.manual_bpm;
    tp_rk.bpm = bpm;
    fxrack_process_i32(&tp_rk, out, frames);      // print the chain into the stream too

    if (recording_is_active()) { recording_push(out); tp.pos += (double)frames; }  // pos = elapsed frames

    float lvl = tp_lvl_eff();                     // Level + CV matrix offset
    for (int f = 0; f < frames; f++) {            // monitor output (level applied last)
        int32_t v = out[f * 2] >> 16;
        float o = (float)v * lvl;
        if (o > 32000.0f) o = 32000.0f; else if (o < -32000.0f) o = -32000.0f;
        int32_t s = ((int32_t)(int16_t)o) << 16;
        out[f * 2] = s; out[f * 2 + 1] = s;
    }
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

    // CV matrix: one value per destination per block, from the same
    // conditioned snapshot the knobs use. Written before the card branch so
    // Level/Cutoff modulation reaches the card-record path too.
    cvmtx_track(&tp.mtx, cvm);
    tp.mx_in  = cvmtx_val(&tp.mtx, cvm, TPM_IN);
    tp.mx_out = cvmtx_val(&tp.mtx, cvm, TPM_OUT);
    tp.mx_win = cvmtx_val(&tp.mtx, cvm, TPM_WIN);
    tp.mx_lvl = cvmtx_val(&tp.mtx, cvm, TPM_LVL);
    tp.mx_cut = cvmtx_val(&tp.mtx, cvm, TPM_CUT);
    // FX param modulation: hand the rack its per-slot offsets (applied inside
    // fxrack_process around each stage; base values stay menu/preset-clean)
    tp_rk.cv1[0] = cvmtx_val(&tp.mtx, cvm, TPM_FX1A);
    tp_rk.cv2[0] = cvmtx_val(&tp.mtx, cvm, TPM_FX1B);
    tp_rk.cv1[1] = cvmtx_val(&tp.mtx, cvm, TPM_FX2A);
    tp_rk.cv2[1] = cvmtx_val(&tp.mtx, cvm, TPM_FX2B);
    tp_rk.cv_rv  = cvmtx_val(&tp.mtx, cvm, TPM_RVMX);

    if (tp.rec_dest == TPD_CARD) { tape_card_process(out, in, io); return; }

    // transport edges: TR1 play/stop, TR2 record punch
    if (io->trig_rising & 1) {
        if (tp.playing) {
            if (tp.recording && tp.rec_extend) {   // stopping a fresh take: finalize + save
                tp.in_pt = 0; tp.out_pt = tp.len; tp.rec_extend = false;
                tape_stash_and_save();
            }
            tp.playing = false; tp.recording = false; tp.rec_stop_target = 0;   // cancel any pending beat-stop
        }
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
        if (gate && !tp.recording) {
            if (tp.playing) tape_rec_start();            // overdub while held
            else            tape_begin_fresh();          // fresh take (auto-saves the old one)
        }
        else if (!gate && tp.recording) tape_rec_stop_request();   // quantize to beat if enabled
    } else {
        if (io->trig_rising & 2) {
            tp.tr2_hold = 0; tp.tr2_armed = false;
            if (tp.recording)    { tape_rec_stop_request(); tp.tr2_overdub = false; }  // punch out (quantized)
            else if (tp.playing) { tape_rec_start();   tp.tr2_overdub = true;  }  // overdub (erasable by holding)
            else                 { tape_begin_fresh(); tp.tr2_overdub = false; }  // stopped: fresh (auto-saves old)
        }
        // While overdubbing, holding TR2 ~0.7 s erases (armed) and the fresh take
        // ROLLS on release, so you place the downbeat when you let go. A stopped
        // TR2 tap already starts a fresh take (saving the old one), so hold-erase
        // is only for converting an overdub into a start-over.
        if (!(io->trig_level & 2)) {                     // gate held (active low)
            if (tp.tr2_overdub) {
                tp.tr2_hold += (uint32_t)(MACHINE_BLOCK / 2);
                if (!tp.tr2_armed && tp.tr2_hold >= (uint32_t)(TP_RATE * 7 / 10)) {
                    tp.tr2_armed = true;
                    tape_erase();                        // wipe now (the loop was saved on its finalize)
                }
            }
        } else {                                         // released
            if (tp.tr2_armed) tape_rec_start();          // roll the fresh take immediately (reliable)
            tp.tr2_hold = 0; tp.tr2_armed = false; tp.tr2_overdub = false;
        }
    }

    uint32_t ein = 0, eout = 0;
    tape_eff_window(&ein, &eout);
    bool empty_rec = tp.rec_extend;                  // latched at punch-in (not per-block)

    float coef = 0, q = 0;
    if (tp.flt_mode != TPF_OFF) {
        float fc = tp_cut_eff();               // Setup cutoff + CV matrix offset
        coef = svf_coef(fc, TP_RATE, 1.0f);
        q = svf_damp(tp.res01, 0.6f, 2.0f);
        if (!(fabsf(tp.flt.lp) < 1e9f) || !(fabsf(tp.flt.bp) < 1e9f)) svf_reset(&tp.flt);
    }

    bool have_tape = tp.tape.nblk > 0;
    // FX apply to INCOMING audio only (monitor + what's printed to tape), NOT to
    // playback — so the chain isn't doubled on top of the already-recorded take.
    bool fx_apply = tp.recording || (!tp.playing && tp.monitor);
    int frames = MACHINE_BLOCK / 2;

    // The record head taps the FULL chain's OUTPUT (prints od/flg/trem/dly/reverb,
    // not just filter/drive). So the frame loop stages the filtered/driven signal
    // into out[] at UNITY (±32767 = the fxchain reference), the block chain runs,
    // THEN we write the post-chain samples to tape and apply the output level LAST.
    // The write PLAN (target frame, len-extend, cap-stop) is decided here and
    // replayed after the chain — it depends only on the playhead, not the sample
    // value, so deferring the actual store is safe.
    uint32_t wpos[MACHINE_BLOCK / 2];
    bool     wdo[MACHINE_BLOCK / 2];
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

        // filter -> drive on the INCOMING audio only (playback stays dry so the FX
        // aren't applied a second time). Stage at UNITY into out[] for the chain.
        float y = src;
        if (fx_apply) {
            if (tp.flt_mode != TPF_OFF) {
                float lp, bp, hp;
                svf_step(&tp.flt, y, coef, q, &lp, &bp, &hp);
                y = tp.flt_mode == TPF_LP ? lp : tp.flt_mode == TPF_BP ? bp : hp;
            }
            if (tp.drive > 0.005f) y = tp_softclip(y, tp.drive);
        }
        float u = y * 32767.0f;
        if (u > 32767.0f) u = 32767.0f; else if (u < -32768.0f) u = -32768.0f;
        int32_t su = ((int32_t)(int16_t)u) << 16;
        out[f * 2] = su;
        out[f * 2 + 1] = su;

        // plan the record write (the POST-CHAIN sample is stored below). Fresh take
        // extends len; overdub writes within the existing loop.
        wdo[f] = false;
        if (tp.recording && have_tape) {
            uint32_t w = (uint32_t)tp.pos;
            if (empty_rec && w < tp.cap) {
                wpos[f] = w; wdo[f] = true;
                if (w + 1 > tp.len) tp.len = w + 1;
                if (w + 1 >= tp.cap) { tape_rec_stop(); tp.playing = false; }   // tape full
            } else if (!empty_rec && w < tp.len) {
                wpos[f] = w; wdo[f] = true;
            }
        }

        // advance + loop the crop window (or the whole fill while first-recording)
        if (tp.playing) {
            tp.pos += 1.0;
            if (!empty_rec) {
                if (tp.pos >= (double)eout || tp.pos >= (double)tp.len) {
                    if (tp.play_oneshot && !tp.recording) tp.playing = false;  // one-shot: stop at end
                    tp.pos = (double)ein;
                }
            }
        }
        // quantized punch-out: finalize once the take reaches the target beat/bar
        if (tp.recording && tp.rec_stop_target && tp.pos >= (double)tp.rec_stop_target)
            tape_rec_stop_at(tp.rec_stop_target);
    }

    // FX rack: FX1/FX2 generic slots (in order) then the fixed reverb. Rate
    // effects (delay/flanger/tremolo) lock to the grid when their Sync is on —
    // Tape feeds the current beat BPM in. INCOMING audio only (dry on playback);
    // the rack unpacks->stages->soft-limits once (fxchain.h) internally.
    float bpm = clockin_beat_bpm(&tp.ci);
    if (bpm <= 0.0f) bpm = tp.manual_bpm;
    if (fx_apply) {
        tp_rk.bpm = bpm;
        fxrack_process_i32(&tp_rk, out, frames);
    }

    // PRINT + output: the record head taps the POST-CHAIN buffer (mono = channel
    // mean) so the whole FX chain is captured to tape. Output level is applied
    // LAST as a master — turning Level down no longer changes what's recorded, and
    // the chain always sees the full-scale reference.
    float lvl = tp_lvl_eff();                     // Level + CV matrix offset
    for (int f = 0; f < frames; f++) {
        int32_t v = ((out[f * 2] >> 16) + (out[f * 2 + 1] >> 16)) >> 1;   // mono print
        if (wdo[f]) tp_wr(wpos[f], (int16_t)v);
        float o = (float)v * lvl * (28000.0f / 32767.0f);
        if (o > 32000.0f) o = 32000.0f; else if (o < -32000.0f) o = -32000.0f;
        int32_t s = ((int32_t)(int16_t)o) << 16;
        out[f * 2] = s;
        out[f * 2 + 1] = s;
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
    tp.take_dirty = false; tp.cropped = false; tp.restore_id[0] = 0;
    return 0;
}

int tape_load(const char *name)
{
    if (!tp_stopped() || tp.tape.nblk == 0 || !name || !name[0]) return -1;
    char path[64];
    if (sample_resolve(name, path, sizeof(path)) != 0) return -2;
    if (tp.take_dirty && tp.len > 1) {              // persist the current take before replacing it
        tape_spawn_save(tp.cropped ? tp.in_pt : 0, tp.cropped ? tp.out_pt : tp.len, tp.cropped);
        while (tp.save_busy) vTaskDelay(pdMS_TO_TICKS(20));
    }

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
    tp.take_dirty = false; tp.cropped = false;     // loaded content isn't an unsaved recording
    snprintf(tp.restore_id, sizeof(tp.restore_id), "%s", name);   // reload this on return
    tape_rebuild_peaks(true);
    return 0;
}

void tape_clear(void)
{
    if (!tp_stopped()) return;
    tp.len = 0; tp.in_pt = tp.out_pt = 0; tp.pos = 0;
    memset(tp.peaks, 0, sizeof(tp.peaks));
    tp.peaks_done = 0;
    tp.take_dirty = false; tp.cropped = false; tp.restore_id[0] = 0;   // explicit wipe = discard
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

// ---- save take -> pool take (background job) -----------------------------------
// Writes frames [save_a, save_b) as usr/<id>.WAV. Manual "Save Crop" and the
// auto-save both stage save_a/save_b/save_crop then spawn this.
static void save_task(void *pv)
{
    (void)pv;
    uint32_t a = tp.save_a, b = tp.save_b;
    char path[48];
    snprintf(path, sizeof(path), "/sdcard/usr/TAPE/%s.WAV", tp.save_id);   // Tape home folder (usr/TAPE now in SF_DIRS)
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
    snprintf(jp, sizeof(jp), "/sdcard/usr/TAPE/%s.JSN", tp.save_id);
    FILE *jf = fopen(jp, "w");
    if (jf) { fputs(tp.save_crop ? "{\"src\":\"tape\",\"crop\":true}"
                                 : "{\"src\":\"tape\",\"crop\":false}", jf); fclose(jf); }
    sd_lock_give();
    heap_caps_free(chunk);
    ESP_LOGI(TAG, "saved take -> %s", tp.save_id);
    tp.save_busy = false;
    vTaskDelete(NULL);
}

// mint a marked id (TCR_ = actively cropped, TAP_ = full take) and spawn the
// writer for [a,b). Must run in a NON-audio task (SD readdir + xTaskCreate).
static int tape_spawn_save(uint32_t a, uint32_t b, bool crop)
{
    if (b <= a || tp.save_busy) return -1;
    const char *pfx = crop ? "TCR_" : "CUT_";
    int idx = sample_next_index(pfx);
    if (idx < 0) idx = 0;
    if (idx > 9999) idx = 9999;                // 8.3: id stays exactly 8 chars
    snprintf(tp.save_id, sizeof(tp.save_id), "%s%04d", pfx, idx % 10000);
    snprintf(tp.restore_id, sizeof(tp.restore_id), "%s", tp.save_id);   // this take is now on card
    tp.save_a = a; tp.save_b = b; tp.save_crop = crop;
    tp.save_busy = true;
    if (xTaskCreate(save_task, "tape_sv", 8192, NULL, 4, NULL) != pdPASS) {
        tp.save_busy = false; tp.save_id[0] = 0; return -2;
    }
    return 0;
}

int tape_save_crop(void)
{
    if (!tp_stopped() || tp.len == 0 || tp.save_busy) return -1;
    crop_clamp();
    int r = tape_spawn_save(tp.in_pt, tp.out_pt, true);   // manual save = the crop, marked
    if (r == 0) tp.take_dirty = false;                    // persisted -> don't re-save on leave
    return r;
}

// UI task: spawn the auto-save the audio task requested (buffer is held intact
// while pending). Called every menu tick.
void tape_autosave_kick(void)
{
    if (!tp.autosave_req || tp.save_busy) return;
    tp.autosave_req = false;
    tape_spawn_save(tp.save_a, tp.save_b, tp.save_crop);
}

// UI task: reload the take persisted on the last leave (CONFIG "tapelast"), so
// work-in-progress survives a machine switch. Silent no-op if empty or the file
// is gone (e.g. the take was deleted from the card).
void tape_restore_last(void)
{
    char id[24];
    if (configGetStringSetting("tapelast", id, sizeof(id)) != 1 || !id[0]) return;
    char path[64];
    if (sample_resolve(id, path, sizeof(path)) != 0) return;   // file no longer on card
    tape_load(id);                                             // sets restore_id + peaks
}

// UI task: in card mode keep the streaming recorder armed so a TR2 punch starts
// instantly (prepare does the heavy SD scan + writer-task create off the audio
// task); also consume the writer's "saved" flag so it can't auto-load into the
// next machine. Cancels a parked writer when card mode is left.
void tape_card_service(void)
{
    int v; char fn[48];
    recording_poll_load(&v, fn);               // clear a completed-take flag (leak guard)
    if (tp.rec_dest != TPD_CARD) {
        if (recording_is_prepared() && !recording_is_active()) recording_cancel_prepared();
        return;
    }
    if (!recording_is_active() && !recording_is_prepared()) {
        recording_set_prefix("TAP");
        recording_prepare(-1);                 // re-arm for the next punch-in
    }
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
fxrack_t tp_rk;                                // pointer-view over tp's FX instances

// CV matrix destinations (order = the TPM_* enum in tape_priv.h). The FX row
// labels are LIVE — refreshed from the rack whenever the CV page opens, so
// "FX1 A" reads as "Dly Mix" when a delay sits in FX1.
const char *tape_mtx_labels[TPM_N] = { "Crop In", "Crop Out", "Window",
                                       "Level", "Cutoff",
                                       "FX1 (off)", "FX1 (off)",
                                       "FX2 (off)", "FX2 (off)", "Rev Mix" };

void tape_mtx_refresh_labels(void)
{
    static const char *const off_lab[2][2] =
        { { "FX1 (off)", "FX1 (off)" }, { "FX2 (off)", "FX2 (off)" } };
    for (int s = 0; s < 2; s++)
        for (int w = 0; w < 2; w++) {
            const char *l = fxrack_cv_label(&tp_rk, s, w);
            tape_mtx_labels[TPM_FX1A + s * 2 + w] = l ? l : off_lab[s][w];
        }
}

static esp_err_t tape_start(void)
{
    memset(&tp, 0, sizeof(tp));
    tp.len_sel = 1;                            // 30 s default
    tp.manual_bpm = 120.0f;
    tp.clk_src = clock_source_clamp_cv_audio(7);
    tp.level = 0.9f;
    tp.cutoff = 2000.0f;
    tp.res01 = 0.1f;
    tp.flt_mode = TPF_OFF;
    tp.monitor = true;
    tp.knob_ctx = -1;
    tp.restore_pending = true;                 // reload the persisted take on first screen entry
    clockin_reset(&tp.ci, 1.0f);
    svf_reset(&tp.flt);
    fxfilter_init(&tp.filt);
    fxfilter_init(&tp.band);
    // Tape's rate effects default to tempo-SYNCED (the tape identity — dub delays
    // lock to the grid); the rack reads tp_rk.bpm per block. Musical-division
    // defaults match the old Tape (1/8 delay, 1/8 tremolo, 1/4 flanger).
    tp.dly.sync = tp.flg.sync = tp.trem.sync = true;
    tp.dly.div = 2; tp.trem.div = 2; tp.flg.div = 4;
    tp_rk = (fxrack_t){ .od = &tp.od, .flg = &tp.flg, .trem = &tp.trem, .dly = &tp.dly,
                        .filt = &tp.filt, .band = &tp.band, .rv = &tp.rv, .slot = tp.fx_slot };
    cvmtx_init(&tp.mtx, (const char *const *)tape_mtx_labels, TPM_N);
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
    tp.pending_fresh = false; tp.autosave_req = false;
    if (recording_is_active()) recording_finish();     // finalize a card take in flight
    recording_cancel_prepared();                       // drop a parked (empty) writer
    while (tp.save_busy) vTaskDelay(pdMS_TO_TICKS(20));   // let any in-flight save finish
    if (tp.take_dirty && tp.len > 1) {                    // leaving Tape: persist the take first
        tape_spawn_save(tp.cropped ? tp.in_pt : 0, tp.cropped ? tp.out_pt : tp.len, tp.cropped);
        while (tp.save_busy) vTaskDelay(pdMS_TO_TICKS(20));
    }
    // remember what's in the buffer so we can reload it on return (empty = nothing)
    configSetStringSetting("tapelast", tp.len > 1 ? tp.restore_id : "");
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
    fxrack_save(&tp_rk, o);                     // slots + every effect param (incl. sync/div)
    cvmtx_save(&tp.mtx, o);                     // CV matrix sources + amounts
    cJSON_AddNumberToObject(o, "lvl", tp.level);
    cJSON_AddNumberToObject(o, "rsrc", tp.rec_src);
    cJSON_AddNumberToObject(o, "rmode", tp.rec_mode);
    cJSON_AddNumberToObject(o, "rdest", tp.rec_dest);
    cJSON_AddNumberToObject(o, "rquant", tp.rec_quant);
    cJSON_AddBoolToObject(o, "mon", tp.monitor);
    cJSON_AddBoolToObject(o, "osht", tp.play_oneshot);
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
    fxrack_load(&tp_rk, node);                  // slots + effect params; migrates legacy on/off bools + divisions
    cvmtx_load(&tp.mtx, node);                  // CV matrix (absent on old presets = all off)
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "lvl"))  && cJSON_IsNumber(j)) tp.level = tp_clampf((float)j->valuedouble, 0, 1.2f);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rsrc")) && cJSON_IsNumber(j)) tp.rec_src = j->valueint == TPS_TAPE ? TPS_TAPE : TPS_INPUT;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rmode")) && cJSON_IsNumber(j)) tp.rec_mode = j->valueint == TPR_MOMENTARY ? TPR_MOMENTARY : TPR_PUNCH;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rdest")) && cJSON_IsNumber(j)) tp.rec_dest = j->valueint == TPD_CARD ? TPD_CARD : TPD_TAPE;
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "rquant")) && cJSON_IsNumber(j)) tp.rec_quant = tp_clampi(j->valueint, 0, TPQ_BAR);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "mon"))  && cJSON_IsBool(j))   tp.monitor = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItemCaseSensitive(node, "osht")) && cJSON_IsBool(j))   tp.play_oneshot = cJSON_IsTrue(j);
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
