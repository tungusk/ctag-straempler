// Deck UI — Live (track, BPMs, rate, beat flash, position bar; press =
// play/pause) and Setup (track/sync/clock/mult-div/loop/BPM/analyze) +
// the shared library browser for track selection.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "audio.h"
#include "sample_ram.h"
#include "deck_priv.h"

static const color_t ACCENT = {40, 200, 230};

static char (*s_samples)[24] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;
static int  s_load_ret = M_DECK_SETUP;   // page the load browser returns to

static void refresh_samples(void){
    // dated list, like the sampler: 512 entries, NEWEST FIRST — a fresh
    // upload is the track you want to cue, and it used to fall off the
    // bottom of a 224-name alphabetical list
    s_n_samples = sample_list_recent(&s_samples);
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], dk.track) == 0) { s_sample_idx = i; break; }
}

// auto-analysis queued behind a still-running previous run (rapid loads)
static void an_auto_poll(void){
    if (dk.an_auto_req && dk.an_state != DK_AN_RUNNING){
        // clear the request only on a successful start — a transient
        // task-create failure used to eat the request and kill auto forever
        if (deck_analyze_start() == 0) dk.an_auto_req = false;
    }
}

// ---- Live -------------------------------------------------------------------
// Redraw discipline: everything is change-driven. Full-region repaints every
// timer tick made the whole screen strobe (Arlo, first hardware test).
static int s_last_barx = -1;
static char s_info1[96] = "", s_info2[64] = "";
static char s_last_track[DK_NAME_LEN] = "";   // detect track changes (e.g. via remote)
static int s_last_dbpm = -1;

// display-side smoothing of the measured external bpm: clock edges are
// quantized to the 64-frame audio block (~1.45 ms), so the raw per-pulse
// figure dances a few tenths even on a crystal-steady clock. EMA over the
// display ticks, snapping through on a real tempo change (>2 bpm).
static float ext_bpm_disp(void){
    static float ema = 0;
    static uint32_t last_tk = 0;
    float x = dk.ci.clk.bpm / DK_PPB_EFF();
    if (!dk.ci.clk.locked || x <= 0) { ema = 0; return x; }
    float d = x - ema;
    if (ema <= 0 || d > 2.0f || d < -2.0f) { ema = x; return ema; }  // real tempo change: snap
    // advance at most 4x/s — this runs from BOTH bpm draws on BOTH timer
    // rates, which multiplied the effective alpha and left the display jumpy
    uint32_t tk = xTaskGetTickCount();
    if (tk - last_tk >= pdMS_TO_TICKS(250)) {
        last_tk = tk;
        ema += 0.08f * d;    // ~3 s settle; raw edge jitter (~±0.35 bpm from
                             // 1.45 ms block quantization) smooths to ~±0.05
    }
    return ema;
}

// the number that matters, BIG: the tempo actually playing right now
// (track bpm x current rate = the external tempo when locked)
static void draw_big_bpm(void){
    // locked: show the TARGET tempo (steady) — the instantaneous rate
    // legitimately wobbles a few % while the PLL corrects phase, which made
    // the big number dance even with a perfect clock
    float bpm;
    if (dk.sync && dk.ci.clk.locked && dk.ci.clk.bpm > 0)
        bpm = ext_bpm_disp() * dk.speed_mult;
    else
        bpm = dk.track_bpm > 0 ? dk.track_bpm * dk.rate : 0;
    int d = (int)(bpm * 10.0f + 0.5f);
    if (d == s_last_dbpm) return;
    s_last_dbpm = d;
    char s[16];
    if (d > 0) snprintf(s, sizeof(s), "%d.%d", d / 10, d % 10);
    else snprintf(s, sizeof(s), "---");
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bw = 120, bh = TFT_getfontheight() + 4;
    _bg = TFT_BLACK;
    TFT_fillRect(_width - bw, 2, bw, bh, _bg);
    _fg = (dk.sync && dk.ci.clk.locked) ? (color_t){40, 200, 90} : TFT_WHITE;
    TFT_print(s, _width - TFT_getStringWidth(s) - 8, 4);
    cfont = f;
}

static void draw_info(void){
    int fh = TFT_getfontheight();
    int y = fh + 50;              // a breath of padding under the big title
    char s1[96], s2[64];
    const char *st = dk.playing ? (dk.loading ? "BUF" : "PLAY") : "STOP";
    const char *sp = dk.speed_mult == 0.5f ? ".5" : (dk.speed_mult == 2.0f ? "2" : "1");
    const char *fl = dk.flt_mode == 1 ? "LP" : (dk.flt_mode == 2 ? "HP" : "");
    // while analysing, the tempo slot shows analysis progress counting UP
    // ("an 16%"), matching the Setup row — shown while playing too (frozen,
    // since analysis pauses during playback) so the pending state stays visible
    if (dk.loop_active){
        // loop_len_beats is in QUARTER-beats: show fractions at the short end
        char lb[12];
        int q = dk.loop_len_beats;
        if (q == 1)      snprintf(lb, sizeof(lb), "1/4");
        else if (q == 2) snprintf(lb, sizeof(lb), "1/2");
        else             snprintf(lb, sizeof(lb), "%d", q / 4);
        snprintf(s1, sizeof(s1), "LOOP %sbt  %s  x%s  %s", lb, st, sp, fl);
    }
    else if (dk.an_state == DK_AN_RUNNING)
        snprintf(s1, sizeof(s1), "ana %d%%  %s  x%s  %s", dk.an_progress, st, sp, fl);
    else
        snprintf(s1, sizeof(s1), "trk %.1f  %s  x%s  %s", dk.track_bpm, st, sp, fl);

    if (dk.sync){
        if (dk.ci.clk.locked) snprintf(s2, sizeof(s2), "ext %.1f bpm  LOCK",
                                    ext_bpm_disp());
        else snprintf(s2, sizeof(s2), "ext: waiting for clock on CV%d", dk.clk_src + 1);
    } else s2[0] = 0;
    if (strcmp(s1, s_info1) != 0){
        strcpy(s_info1, s1);
        _bg = TFT_BLACK; TFT_fillRect(0, y, _width, fh + 4, _bg);
        _fg = TFT_WHITE;
        TFT_print(s1, 8, y);
    }
    if (strcmp(s2, s_info2) != 0){
        strcpy(s_info2, s2);
        _bg = TFT_BLACK; TFT_fillRect(0, y + fh + 6, _width, fh + 4, _bg);
        _fg = dk.ci.clk.locked ? (color_t){40, 200, 90} : TFT_LIGHTGREY;
        if (s2[0]) TFT_print(s2, 8, y + fh + 6);
    }
}

// big transport: full-width bar right under the info lines. The bar's
// BACKGROUND is the transport state — green while playing, blue while
// stopped (the separate beat-flash lamp never quite read as "on the beat"
// visually, so it's gone; this is simpler and unambiguous)
#define TBAR_X 8
#define TBAR_Y 112
#define TBAR_H 30
#define DK_NUDGE_STEP 0.01f   // phase nudge per detent (~1.25ms @ 4PPQN/120bpm)
#define TBAR_W (_width - 16)

static int s_bar_state = -1;   // -1 forces the first paint

static int tbar_state(void){   // 1 = playing, 2 = analyzing (stopped), 0 = idle
    // playing wins: analysis is paused during playback, so show green not pink.
    // LOOPING is NOT a colour state (Arlo): the box keeps the transport colour
    // and the SHRINK to the window is the whole signal.
    return dk.playing ? 1 : (dk.an_state == DK_AN_RUNNING ? 2 : 0);
}
static color_t tbar_bg(void){
    switch (tbar_state()){
        case 2:  return (color_t){210, 70, 150};   // pink: analysis running
        case 1:  return (color_t){25, 120, 50};    // green: playing
        default: return (color_t){30, 60, 140};    // blue: stopped
    }
}

// transport bar, restyled (Arlo): BLACK canvas so the waveform pops, with the
// transport state on a FAT color-coded border instead of the fill. The marker
// erase repaints its interior slice: black + the waveform columns that fall
// inside it.
#define TBAR_BW 3     // fat state border
static const color_t WF_GREY = {125, 125, 135};   // waveform: reads under the white playhead

static void tbar_paint_slice(int x, int w){
    TFT_fillRect(x, TBAR_Y + TBAR_BW, w, TBAR_H - 2 * TBAR_BW, (color_t){0, 0, 0});
    if (dk.wf_state == 2){
        int wx = TBAR_X + TBAR_BW + 1, ww = TBAR_W - 2 * TBAR_BW - 2;
        int wy = TBAR_Y + TBAR_BW + 1, wh = TBAR_H - 2 * TBAR_BW - 2;
        // BOLD: 2px strokes, MEDIUM GREY (Arlo) — white waveform columns
        // swallowed the white position marker in a dense track. sqrt amplitude
        // lift (1px strokes read washed-out inside the box).
        color_t wc = WF_GREY;
        for (int c = 0; c < ww - 1; c += 2){
            int px = wx + c;
            if (px + 2 <= x || px >= x + w) continue;
            float a = sqrtf((float)dk.wf[(c * DK_WF_W) / ww] / 255.0f);
            int h = (int)(a * (float)wh);
            if (h < 2) h = 2;
            TFT_fillRect(px, wy + (wh - h) / 2, 2, h, wc);
        }
    }
}

static int s_wf_drawn = -1;    // wf_state the bar was last painted with
static int s_loop_key = -1;    // loop window fingerprint (band change detection)

// THE BOX IS THE LOOP (Arlo, ear test: "shrink the bar like on the sampler").
// While looping, the fat state-colored border stops framing the whole track
// and SHRINKS to frame the loop window — the same idiom as sampler3's crop
// box. The waveform keeps running full width behind it, so you see where the
// window sits in the track at a glance. Clipped to [sx, sx+sw) so the
// playhead-erase slices restore it.
static void tbar_loop_box(int sx, int sw){
    if (!dk.loop_active || !dk.file_frames) return;
    int bx = TBAR_X + TBAR_BW + 1, bw = TBAR_W - 2 * TBAR_BW - 6;
    int x0 = bx + (int)((uint64_t)dk.loop_start * bw / dk.file_frames);
    int x1 = bx + (int)((uint64_t)(dk.loop_start + dk.loop_len_fr) * bw / dk.file_frames);
    if (x1 < x0 + 2 * TBAR_BW) x1 = x0 + 2 * TBAR_BW;   // stay legible when tiny
    color_t pk = tbar_bg();          // same colour as the full bar (Arlo)
    struct { int x, w, y, h; } seg[4] = {
        { x0, x1 - x0, TBAR_Y, TBAR_BW },                       // top
        { x0, x1 - x0, TBAR_Y + TBAR_H - TBAR_BW, TBAR_BW },    // bottom
        { x0, TBAR_BW, TBAR_Y, TBAR_H },                        // left end
        { x1 - TBAR_BW, TBAR_BW, TBAR_Y, TBAR_H },              // right end
    };
    for (int k = 0; k < 4; k++){
        int a = seg[k].x > sx ? seg[k].x : sx;
        int b = (seg[k].x + seg[k].w) < (sx + sw) ? (seg[k].x + seg[k].w) : (sx + sw);
        if (b > a) TFT_fillRect(a, seg[k].y, b - a, seg[k].h, pk);
    }
}

// engage/release + window moves all live in this key — tbar_state no longer
// changes on loop, so THIS is what triggers the frame repaint
static int loop_key(void){
    return dk.loop_active
        ? (int)(dk.loop_start >> 11) * 31 + (int)(dk.loop_len_fr >> 11) : -1;
}

static void draw_posbar_frame(void){
    if (dk.loop_active){
        // no full-width border while looping: the canvas goes edge to edge and
        // the ONLY box is the loop window (drawn by the caller). Black frame
        // fill wipes the previous full border away.
        TFT_fillRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, (color_t){0, 0, 0});
        tbar_paint_slice(TBAR_X, TBAR_W);
    } else {
        TFT_fillRect(TBAR_X, TBAR_Y, TBAR_W, TBAR_H, tbar_bg());   // border color
        tbar_paint_slice(TBAR_X + TBAR_BW, TBAR_W - 2 * TBAR_BW);  // black canvas
    }
    _bg = TFT_BLACK;      // _bg is a shared global — leaking the bar color
                          // painted the hint line's text background blue
    s_last_barx = -1;
    s_bar_state = tbar_state();
    s_wf_drawn = dk.wf_state;
}

static void draw_posbar(void){
    if (tbar_state() != s_bar_state ||        // border follows transport;
        (dk.wf_state == 2) != (s_wf_drawn == 2) ||  // thumbnail landed: paint it
        loop_key() != s_loop_key){                  // loop window moved/resized
        draw_posbar_frame();
        tbar_loop_box(TBAR_X, TBAR_W);
        s_loop_key = loop_key();
    }
    if (!dk.file_frames) return;
    int x = TBAR_X + TBAR_BW + 1 +
            (int)((uint64_t)dk.ui_fpos * (TBAR_W - 2 * TBAR_BW - 6) / dk.file_frames);
    if (x == s_last_barx) return;
    if (s_last_barx > 0){                     // erase only the old marker slice
        tbar_paint_slice(s_last_barx, 5);
        tbar_loop_box(s_last_barx, 5);        // restore the box through it
    }
    TFT_fillRect(x, TBAR_Y + TBAR_BW, 5, TBAR_H - 2 * TBAR_BW, (color_t){245, 245, 245});
    s_last_barx = x;
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Deck", 6, 4);
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    char nm[16];
    snprintf(nm, sizeof(nm), "%.9s", dk.track[0] ? dk.track : "(none)");
    TFT_print(nm, 8, TFT_getfontheight() + 4);
    cfont = f;
    strlcpy(s_last_track, dk.track, sizeof(s_last_track));
    s_last_dbpm = -1;
    draw_big_bpm();
    s_info1[0] = 0;
    s_info2[0] = 0;
    draw_info();
    s_bar_state = -1;        // force the first frame paint
    draw_posbar();
    // total track length, right-justified under the transport bar
    if (dk.file_frames){
        char tl[12];
        unsigned ts = (unsigned)(dk.file_frames / DK_RATE);
        snprintf(tl, sizeof(tl), "%u:%02u", ts / 60, ts % 60);
        _fg = TFT_LIGHTGREY;
        _bg = TFT_BLACK;
        TFT_print(tl, TBAR_X + TBAR_W - TFT_getStringWidth(tl), TBAR_Y + TBAR_H + 4);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("turn:scrub/nudge press:tracks TR1:play TR2:loop", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int deck_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW: {
            an_auto_poll();
            if (strcmp(dk.track, s_last_track) != 0) {   // track changed under us (remote)
                live_full_redraw();                      // repaints name + resyncs caches
            } else {
                draw_big_bpm();
                draw_info();
                draw_posbar();
            }
            if (event == EV_TIMER_REPEATING_SLOW){
                // engine internals through /status (v1) for remote debugging
                char dbg[56];
                snprintf(dbg, sizeof(dbg), "%c e%lu i%lu p%lu E%+d S%lu g%u L%d W%ld",
                         dk.loop_active ? 'L' : (dk.playing ? 'P' : 's'),
                         (unsigned long)dk.ci.raw_fires,
                         (unsigned long)(dk.ci.raw_iv / 44),        // ms between fires
                         (unsigned long)(dk.ci.clk.period / 44),    // ms accepted period
                         (int)(dk.phase_err * 100),              // PLL convergence
                         (unsigned long)dk.dbg_starve,           // ring underrun blocks
                         (unsigned)dk.ci.clk.ghost_run,             // escape-hatch state
                         (int)dk.ci.clk.locked,
                         (long)((int32_t)(dk.wpos - dk.rpos_i)));   // ring LEAD
                audio_status_set_voices("deck", dbg);
            }
            break;
        }
        case EV_SHORT_PRESS:                         // press = track browser
            refresh_samples();                       // (TR1/TR2 are the transport)
            s_load_ret = M_DECK_LIVE;
            return M_DECK_LOAD;
        case EV_FWD:
            if (dk.ci.clk.locked && dk.sync) {          // locked+synced: fine phase nudge
                dk.phase_offset -= DK_NUDGE_STEP;
                if (dk.phase_offset < 0.0f) dk.phase_offset += 1.0f;
            } else deck_seek_beats(+4);              // else: one-bar scrub
            break;
        case EV_BWD:
            if (dk.ci.clk.locked && dk.sync) {
                dk.phase_offset += DK_NUDGE_STEP;
                if (dk.phase_offset >= 1.0f) dk.phase_offset -= 1.0f;
            } else deck_seek_beats(-4);
            break;
        case EV_LONG_PRESS: return M_DECK_SETUP;   // toggle Live -> Setup (no hub)
        default: break;
    }
    return 0;
}

// ---- Setup --------------------------------------------------------------------
static const char *setup_labels[] = {"Track", "Sync", "Clock Src", "Clock", "Loop", "BPM", "Grid Nudge", "Analyze", "Auto BPM", "Loop Freeze", "Feel", "Clk Scale"};
#define DK_SETUP_N 12

// house Setup grammar (sampler3 pilot, now everywhere): TOGGLES and short
// cycles flip right on the click; lists/ranges keep click-to-edit and show an
// explicit "[ value ]" bracket while editing. Value edits repaint ONE row.
#define SETUP_IS_TOGGLE(i) ((i) == 1 || (i) == 4 || (i) == 8 || (i) == 9 || \
                            (i) == 10 || (i) == 11)
#define SETUP_ROW_Y(i) (TFT_getfontheight() + 14 + (i) * (TFT_getfontheight() + 7))

static void setup_value_str(int i, char *v, size_t n){
    switch(i){
        case 0: snprintf(v, n, "%s", dk.track[0] ? dk.track : "(none)"); break;
        case 1: snprintf(v, n, "%s", dk.sync ? "ON" : "OFF"); break;
        case 2: snprintf(v, n, "CV%d", dk.clk_src + 1); break;
        case 3: snprintf(v, n, "%s", dk_ppb_names[dk.ppb_idx]); break;
        case 4: snprintf(v, n, "%s", dk.loop ? "ON" : "OFF"); break;
        case 5:
            if (dk.track_bpm > 0) snprintf(v, n, "%.1f", dk.track_bpm);
            else snprintf(v, n, "?");
            break;
        case 6: snprintf(v, n, "%lums", (unsigned long)(dk.grid_offset * 1000 / DK_RATE)); break;
        case 7:
            if (dk.an_state == DK_AN_RUNNING) snprintf(v, n, "%d%%", dk.an_progress);
            else if (dk.an_state == DK_AN_DONE) snprintf(v, n, "%.1f bpm", dk.an_bpm);
            else if (dk.an_state == DK_AN_FAIL) snprintf(v, n, "FAILED");
            else snprintf(v, n, "press");
            break;
        case 8: snprintf(v, n, "%s", dk.auto_an ? "ON" : "OFF"); break;
        case 9: snprintf(v, n, "%s", dk.loop_freeze ? "ON" : "OFF"); break;
        case 10: snprintf(v, n, dk.feel == 0.5f ? "x0.5" : dk.feel == 2.0f ? "x2" : "x1"); break;
        case 11: snprintf(v, n, dk.clk_scale == 0.5f ? "x0.5" : dk.clk_scale == 2.0f ? "x2" : "x1"); break;
    }
}

static void setup_row_redraw(int i, int pos, int sel){
    int fh = TFT_getfontheight();
    int y = SETUP_ROW_Y(i);
    bool editing = (i == pos && sel);
    _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
    _fg = editing ? TFT_CYAN : TFT_WHITE;
    TFT_fillRect(0, y - 2, _width, fh + 5, _bg);
    TFT_print((char*)setup_labels[i], 8, y);
    char raw[28], val[32];
    setup_value_str(i, raw, sizeof(raw));
    if (editing) snprintf(val, sizeof(val), "[ %s ]", raw);   // edit-mode bracket
    else snprintf(val, sizeof(val), "%s", raw);
    TFT_print(val, _width - TFT_getStringWidth(val) - 10, y);
    _bg = TFT_BLACK;
}

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    _fg = TFT_WHITE;
    TFT_print("Deck Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);   // top-right; pos -1 = System
    for (int i = 0; i < DK_SETUP_N; i++) setup_row_redraw(i, pos, sel);
    // no hint line: the row list grew (Feel / Clk Scale) and the hint was
    // overlapping the last rows (Arlo)
}

static void setup_adj(int i, int dir){
    switch(i){
        case 1: dk.sync = !dk.sync; break;
        case 2: dk.clk_src = (dk.clk_src + (dir > 0 ? 1 : 7)) & 7; break;
        case 3: dk.ppb_idx += dir; if (dk.ppb_idx < 0) dk.ppb_idx = 0; if (dk.ppb_idx > 5) dk.ppb_idx = 5; break;
        case 4: dk.loop = !dk.loop; break;
        case 5: {
            float b = dk.track_bpm > 0 ? dk.track_bpm : 120.0f;
            b += dir * 0.5f;
            if (b < 40) b = 40;
            if (b > 240) b = 240;
            dk.track_bpm = b;
            break;
        }
        case 6: {
            int32_t g = (int32_t)dk.grid_offset + dir * (DK_RATE / 100);   // ±10 ms
            if (g < 0) g = 0;
            dk.grid_offset = (uint32_t)g;
            break;
        }
        case 8: dk.auto_an = !dk.auto_an; break;
        case 9: dk.loop_freeze = !dk.loop_freeze; break;
        case 10: {   // per-track feel: x0.5 <-> x1 <-> x2, persisted in the sidecar
            float f = dk.feel;
            f = (dir > 0) ? (f == 0.5f ? 1.0f : 2.0f) : (f == 2.0f ? 1.0f : 0.5f);
            deck_set_feel(f);
            break;
        }
        case 11: {   // clock scale: the friendly layer over ppb
            float c = dk.clk_scale;
            c = (dir > 0) ? (c == 0.5f ? 1.0f : 2.0f) : (c == 2.0f ? 1.0f : 0.5f);
            dk.clk_scale = c;
            break;
        }
    }
}

static int deck_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_TIMER_REPEATING_SLOW: {
            an_auto_poll();
            // repaint per tick only while analysis runs (progress %); paint
            // DONE/FAIL once on the transition — DONE persists after an
            // analysis, and repainting it every tick strobed the screen
            static int s_an_drawn = DK_AN_IDLE;
            if (dk.an_state == DK_AN_RUNNING || dk.an_state != s_an_drawn){
                s_an_drawn = dk.an_state;
                setup_redraw(pos, sel);
            }
            break;
        }
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if(sel){
                setup_adj(pos, dir);
                setup_row_redraw(pos, pos, sel);      // value edit: ONE row
            } else {
                pos += dir;
                if(pos >= DK_SETUP_N) pos = -1;       // past bottom -> System
                if(pos < -1) pos = DK_SETUP_N - 1;    // past System -> bottom
                setup_redraw(pos, sel);
            }
            break;
        }
        case EV_SHORT_PRESS:
            if(pos == -1) return M_MORE;                       // System affordance
            if(pos == 0){ refresh_samples(); s_load_ret = M_DECK_SETUP; return M_DECK_LOAD; }
            if(pos == 7){ deck_analyze_start(); setup_redraw(pos, sel); break; }
            if(SETUP_IS_TOGGLE(pos)){
                setup_adj(pos, +1);                   // toggles flip on the click
                setup_row_redraw(pos, pos, 0);
            } else {
                sel = !sel;                           // lists/ranges: edit mode
                setup_row_redraw(pos, pos, sel);
            }
            break;
        case EV_LONG_PRESS: return M_DECK_LIVE;   // toggle Setup -> Live (no hub)
        default: break;
    }
    return 0;
}

// ---- Load browser ---------------------------------------------------------------
static void load_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[48];
    snprintf(h, sizeof(h), "Load Track  (%d/%d)", s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    if (!s_n_samples){
        char *m = "no files in usr/";
        TFT_print(m, _width / 2 - TFT_getStringWidth(m) / 2, _height / 2);
        return;
    }
    int cy = _height / 2;
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int bigfh = TFT_getfontheight();
    _fg = TFT_WHITE;
    char *selnm = s_samples[s_sample_idx];
    TFT_print(selnm, _width / 2 - TFT_getStringWidth(selnm) / 2, cy - bigfh / 2);
    cfont = f;
    _fg = (color_t){110, 110, 110};
    for (int k = 1; k <= 4; k++){
        int up = s_sample_idx - k, dn = s_sample_idx + k;
        int yup = cy - bigfh / 2 - k * (fh + 4) - 4;
        int ydn = cy + bigfh / 2 + (k - 1) * (fh + 4) + 6;
        if (up >= 0){ char *nn = s_samples[up]; TFT_print(nn, _width / 2 - TFT_getStringWidth(nn) / 2, yup); }
        if (dn < s_n_samples){ char *nn = s_samples[dn]; TFT_print(nn, _width / 2 - TFT_getStringWidth(nn) / 2, ydn); }
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:browse  press:load  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int deck_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: load_redraw(); break;
        case EV_FWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + 1) % s_n_samples; load_redraw(); } break;
        case EV_BWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + s_n_samples - 1) % s_n_samples; load_redraw(); } break;
        case EV_SHORT_PRESS:
            // re-selecting the loaded track is a no-op (don't reload/re-settle)
            if(s_n_samples && strcmp(s_samples[s_sample_idx], dk.track) != 0)
                deck_load_track(s_samples[s_sample_idx]);
            return s_load_ret;
        case EV_LONG_PRESS:
            return s_load_ret;
        default: break;
    }
    return 0;
}

// ---- registration -----------------------------------------------------------------
static void deck_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_DECK_LIVE);  menusys_item_set_default_cb(_ms, M_DECK_LIVE, deck_live_handler);
    menusys_new_item(_ms, M_DECK_SETUP); menusys_item_set_default_cb(_ms, M_DECK_SETUP, deck_setup_handler);
    menusys_new_item(_ms, M_DECK_LOAD);  menusys_item_set_default_cb(_ms, M_DECK_LOAD, deck_load_handler);
}

static int deck_main_event(int event, void *ev_data){
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW){
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = ACCENT;
        char s[64];
        snprintf(s, sizeof(s), "Deck: %.12s %s %.0f bpm",
                 dk.track[0] ? dk.track : "(none)",
                 dk.playing ? "playing" : "stopped", dk.track_bpm);
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const deck_main_items[] = {"Live", "Setup"};
static const int deck_main_targets[] = {M_DECK_LIVE, M_DECK_SETUP};

const machine_ui_t deck_menu_ui = {
    .main_items = deck_main_items,
    .main_targets = deck_main_targets,
    .n_main = 2,
    .register_pages = deck_register_pages,
    .main_event = deck_main_event,
    .boot_target = M_DECK_LIVE,
};
