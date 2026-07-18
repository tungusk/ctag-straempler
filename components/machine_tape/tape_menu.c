// Tape UI — the screen IS the editor: one big waveform (the whole tape) with
// the crop window boxed bright, material outside dimmed, a beat grid anchored
// at the IN point, and a white (red while recording) playhead. Encoder: turn
// moves the selected cursor (grid-snapped when a clock/BPM is known, zero-
// cross otherwise), press cycles IN -> OUT -> WIN (slide both), hold = Setup.
// TR1 = play/stop, TR2 = record punch. Everything else lives in Setup.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "setup_menu.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "sample_browser.h"
#include "sample_ram.h"
#include "tape_priv.h"

static const color_t WF_DIM   = {70, 70, 80};     // outside the crop
static const color_t WF_LIT   = {235, 235, 240};  // inside the crop
static const color_t CROP_COL = {70, 200, 235};   // crop edges (cyan)
static const color_t GRID_COL = {40, 52, 70};     // beat ticks
static const color_t BAR_COL  = {60, 80, 105};    // every 4th beat
static const color_t PH_COL   = {240, 240, 245};
static const color_t REC_COL  = {230, 60, 50};

// ---- geometry ----------------------------------------------------------------
#define W_X 8
#define W_W 300
static int w_y(void) { return TFT_getfontheight() + 10; }
static int w_h(void) { return 150; }

static int s_sel = 0;                 // 0 = IN, 1 = OUT, 2 = WIN
static int s_last_ph = -1;
static unsigned s_sig_head = 0, s_sig_crop = 0;
static uint32_t s_wave_len = 0;       // len at last wave draw (record growth)

static int frame_x(uint32_t fr)
{
    if (tp.cap == 0) return W_X;
    if (fr > tp.cap) fr = tp.cap;
    return W_X + (int)((uint64_t)fr * W_W / tp.cap);
}

static void fmt_secs(uint32_t fr, char *b, size_t n)
{
    snprintf(b, n, "%.2fs", (float)fr / (float)TP_RATE);
}

// ---- drawing -------------------------------------------------------------------
static void draw_header(void)
{
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; TFT_fillRect(0, 0, _width, fh + 8, _bg);
    _fg = TFT_WHITE; TFT_print("Tape", 6, 3);
    // state chip
    const char *st = tp.recording ? "REC" : tp.playing ? "PLAY" : "STOP";
    color_t sc = tp.recording ? REC_COL : tp.playing ? (color_t){40, 200, 90} : (color_t){120, 120, 130};
    _fg = sc; TFT_print((char *)st, 52, 3);
    // position / extent
    char b[40];
    snprintf(b, sizeof(b), "%.1f/%.0fs", (float)tp.pos / TP_RATE, (float)tp.cap / TP_RATE);
    _fg = (color_t){140, 140, 150};
    TFT_print(b, 108, 3);
    // grid bpm + source tag
    tape_beat_frames();
    snprintf(b, sizeof(b), "%.1f %s", tp.disp_bpm, tp.disp_clk ? "CLK" : "man");
    _fg = tp.disp_clk ? (color_t){40, 200, 90} : (color_t){120, 120, 130};
    TFT_print(b, _width - 8 - TFT_getStringWidth(b), 3);
}

// one waveform column (also used to erase the playhead)
static void wave_col(int x)
{
    int y0 = w_y(), h = w_h(), cy = y0 + h / 2;
    if (x < W_X || x >= W_X + W_W) return;
    _bg = TFT_BLACK; TFT_fillRect(x, y0, 1, h, _bg);
    uint32_t ein, eout; tape_eff_window(&ein, &eout);
    int xi = frame_x(ein), xo = frame_x(eout);
    // grid tick at this column?
    if (tp.len) {
        uint32_t b = tape_beat_frames();
        if (b > 4410 / 2) {                     // draw grid only when ticks >= ~4px apart
            long fr = (long)((uint64_t)(x - W_X) * tp.cap / W_W);
            long rel = fr - (long)tp.in_pt;
            long bi = rel >= 0 ? rel / (long)b : (rel - (long)b + 1) / (long)b;
            long tick = (long)tp.in_pt + bi * (long)b;
            int tx = frame_x((uint32_t)(tick < 0 ? 0 : tick));
            int tx2 = frame_x((uint32_t)(tick + (long)b));
            if (x == tx || x == tx2) {
                long bidx = (x == tx) ? bi : bi + 1;
                TFT_drawLine(x, y0, x, y0 + h, (bidx % 4 == 0) ? BAR_COL : GRID_COL);
            }
        }
    }
    int pi = (x - W_X) * TP_PEAKS / W_W;
    if (pi >= 0 && pi < TP_PEAKS && tp.peaks[pi]) {
        int ph = tp.peaks[pi] * (h / 2 - 2) / 255;
        if (ph < 1) ph = 1;
        long fr = (long)((uint64_t)(x - W_X) * tp.cap / W_W);
        bool inside = fr >= (long)ein && fr < (long)eout;
        TFT_drawLine(x, cy - ph, x, cy + ph, inside ? WF_LIT : WF_DIM);
    } else {
        TFT_drawPixel(x, cy, (color_t){45, 45, 52}, 1);   // baseline
    }
    if (x == xi || x == xo) TFT_drawLine(x, y0, x, y0 + h, CROP_COL);
}

static void draw_wave(void)
{
    int y0 = w_y(), h = w_h();
    _bg = TFT_BLACK; TFT_fillRect(W_X - 2, y0, W_W + 4, h, _bg);
    TFT_drawRect(W_X - 2, y0 - 1, W_W + 4, h + 2, (color_t){36, 42, 56});
    if (tp.len == 0) {
        _fg = (color_t){90, 90, 100};
        TFT_print("empty tape - TR2/rec records line-in,", W_X + 16, y0 + h / 2 - TFT_getfontheight());
        TFT_print("or Setup > Load Sample", W_X + 16, y0 + h / 2 + 2);
        s_last_ph = -1;
        s_wave_len = 0;
        return;
    }
    for (int x = W_X; x < W_X + W_W; x++) wave_col(x);
    s_last_ph = -1;
    s_wave_len = tp.len;
}

static void draw_playhead(void)
{
    int y0 = w_y(), h = w_h();
    int ph = -1;
    if (tp.len || tp.recording) ph = frame_x((uint32_t)tp.pos);
    if (ph == s_last_ph && !tp.recording) return;
    if (s_last_ph >= 0 && s_last_ph != ph) wave_col(s_last_ph);
    if (ph >= 0) TFT_drawLine(ph, y0, ph, y0 + h, tp.recording ? REC_COL : PH_COL);
    s_last_ph = ph;
}

static void draw_readout(void)
{
    int fh = TFT_getfontheight();
    int y = w_y() + w_h() + 6;
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, fh + 6, _bg);
    char a[16], b[16], c[48];
    fmt_secs(tp.in_pt, a, sizeof(a));
    fmt_secs(tp.out_pt, b, sizeof(b));
    uint32_t bt = tape_beat_frames();
    float beats = bt ? (float)(tp.out_pt - tp.in_pt) / (float)bt : 0;
    const char *tag[3] = { "IN", "OUT", "WIN" };
    int x = 8;
    for (int i = 0; i < 2; i++) {
        char seg[32];
        snprintf(seg, sizeof(seg), "%s%s %s%s", s_sel == i ? "[" : "", tag[i],
                 i == 0 ? a : b, s_sel == i ? "]" : "");
        _fg = s_sel == i ? TFT_CYAN : (color_t){150, 150, 160};
        TFT_print(seg, x, y + 2);
        x += TFT_getStringWidth(seg) + 12;
    }
    snprintf(c, sizeof(c), "%s%.1f beats%s", s_sel == 2 ? "[" : "", beats, s_sel == 2 ? "]" : "");
    _fg = s_sel == 2 ? TFT_CYAN : (color_t){110, 110, 120};
    TFT_print(c, x, y + 2);
}

static void draw_footer(void)
{
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    _bg = TFT_BLACK;
    TFT_fillRect(0, _height - TFT_getfontheight() - 2, _width, TFT_getfontheight() + 2, _bg);
    TFT_print("turn:move press:in/out/win hold:setup  TR1:play TR2:rec", 6,
              _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static unsigned head_sig(void)
{
    return (tp.playing ? 1u : 0u) + (tp.recording ? 2u : 0u)
         + (unsigned)tp.disp_bpm * 8u + ((unsigned)((float)tp.pos / TP_RATE) << 16);
}
static unsigned crop_sig(void)
{
    uint32_t ein, eout; tape_eff_window(&ein, &eout);
    return ein * 2654435761u ^ eout * 40503u ^ (unsigned)s_sel;
}

static void main_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    draw_header();  s_sig_head = head_sig();
    draw_wave();
    draw_playhead();
    draw_readout(); s_sig_crop = crop_sig();
    draw_footer();
}

// ---- cursor edits ---------------------------------------------------------------
static void nudge(int dir)
{
    if (tp.len == 0) return;
    uint32_t bt = tape_beat_frames();
    long step = (tp.disp_bpm > 0) ? (long)bt : (long)(tp.len / 200 + 1);
    long d = (long)dir * step;
    if (s_sel == 0) {                   // IN (grid re-anchors with it)
        long v = (long)tp.in_pt + d;
        v = v < 0 ? 0 : v;
        if (v > (long)tp.out_pt - 64) v = (long)tp.out_pt - 64;
        if (tp.disp_bpm <= 0) v = (long)tape_snap((uint32_t)(v < 0 ? 0 : v));
        tp.in_pt = (uint32_t)(v < 0 ? 0 : v);
    } else if (s_sel == 1) {            // OUT (snaps to the IN-anchored grid)
        long v = (long)tp.out_pt + d;
        if (v < (long)tp.in_pt + 64) v = (long)tp.in_pt + 64;
        if (v > (long)tp.len) v = (long)tp.len;
        v = (long)tape_snap((uint32_t)v);
        if (v <= (long)tp.in_pt) v = (long)tp.in_pt + 64;
        if (v > (long)tp.len) v = (long)tp.len;
        tp.out_pt = (uint32_t)v;
    } else {                            // WIN: slide both, width fixed
        long w = (long)tp.out_pt - (long)tp.in_pt;
        long i = (long)tp.in_pt + d;
        if (i < 0) i = 0;
        if (i + w > (long)tp.len) i = (long)tp.len - w;
        if (i < 0) i = 0;
        tp.in_pt = (uint32_t)i;
        tp.out_pt = (uint32_t)(i + w);
    }
}

static int tape_main_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: main_full_redraw(); break;
        case EV_FWD: nudge(+1); draw_wave(); draw_readout(); s_sig_crop = crop_sig(); break;
        case EV_BWD: nudge(-1); draw_wave(); draw_readout(); s_sig_crop = crop_sig(); break;
        case EV_SHORT_PRESS:
            s_sel = (s_sel + 1) % 3;
            draw_readout(); s_sig_crop = crop_sig();
            break;
        case EV_LONG_PRESS: return M_TAPE_SETUP;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST: {
            if (tp.recording && tp.len > s_wave_len) {   // live record growth
                tape_rebuild_peaks(false);
                draw_wave();
            }
            unsigned hs = head_sig();
            if (hs != s_sig_head) { draw_header(); s_sig_head = hs; }
            unsigned cs = crop_sig();
            if (cs != s_sig_crop) { draw_wave(); draw_readout(); s_sig_crop = cs; }
            draw_playhead();
            break;
        }
        default: break;
    }
    return 0;
}

// ---- Setup (shared framework) ------------------------------------------------
static const int BEAT_LADDER[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
#define BEAT_LADDER_N 10
static int s_beats_idx = 3;           // "4 beats"

// Tape/transport params + edit actions live here; all five effects moved to a
// dedicated FX sub-page (the "FX" action row) so this list stays readable.
static const setup_item_t tape_setup_items[] = {
    {"Crop Beats", ST_RANGE},  {"Clock Src",  ST_TOGGLE}, {"Manual BPM", ST_RANGE},
    {"Filter",     ST_TOGGLE}, {"Cutoff",     ST_RANGE},  {"Reso",       ST_RANGE},
    {"Drive",      ST_RANGE},  {"Level",      ST_RANGE},  {"Rec Source", ST_TOGGLE},
    {"Monitor",    ST_TOGGLE}, {"FX",         ST_ACTION},
    {"Copy",       ST_ACTION}, {"Cut",        ST_ACTION}, {"Paste",      ST_ACTION},
    {"Normalize",  ST_ACTION}, {"Reverse",    ST_ACTION}, {"Fade Edges", ST_ACTION},
    {"Save Crop",  ST_ACTION}, {"Load Sample", ST_ACTION}, {"Clear Tape", ST_ACTION},
    {"Tape Len",   ST_TOGGLE},
};

// count of engaged effects, for the "FX" row affordance
static int tape_fx_count(void)
{
    return (tp.rv.mode != RV_OFF) + (tp.dly_on ? 1 : 0) + (tp.od_on ? 1 : 0)
         + (tp.flg_on ? 1 : 0) + (tp.trem_on ? 1 : 0);
}

static const char *stopped_or(const char *s) { return (!tp.playing && !tp.recording) ? s : "stop first"; }

static void tape_val(int i, char *v, size_t n)
{
    switch (i) {
        case 0: snprintf(v, n, "%d", BEAT_LADDER[s_beats_idx]); break;
        case 1: snprintf(v, n, "%s", clock_source_name(tp.clk_src)); break;
        case 2: snprintf(v, n, "%.0f", tp.manual_bpm); break;
        case 3: snprintf(v, n, "%s", tp.flt_mode == TPF_LP ? "LP" : tp.flt_mode == TPF_BP ? "BP" :
                                     tp.flt_mode == TPF_HP ? "HP" : "off"); break;
        case 4: {
            if (tp.cutoff >= 1000) snprintf(v, n, "%.1fk", tp.cutoff / 1000);
            else                   snprintf(v, n, "%.0f", tp.cutoff);
            break;
        }
        case 5: snprintf(v, n, "%.0f%%", tp.res01 * 100); break;
        case 6: snprintf(v, n, "%.0f%%", tp.drive * 100); break;
        case 7: snprintf(v, n, "%.0f%%", tp.level * 100); break;
        case 8: snprintf(v, n, "%s", tp.rec_src == TPS_TAPE ? "tape (print)" : "input"); break;
        case 9: snprintf(v, n, "%s", tp.monitor ? "ON" : "OFF"); break;
        case 10: { int on = tape_fx_count(); if (on) snprintf(v, n, "%d on >", on); else snprintf(v, n, "off >"); break; }
        case 11: {
            if (tp.clip_len) snprintf(v, n, "%.2fs held", (float)tp.clip_len / TP_RATE);
            else             snprintf(v, n, "%s", stopped_or("copy >"));
            break;
        }
        case 12: snprintf(v, n, "%s", stopped_or("cut >")); break;
        case 13: {
            if (!tp.clip_len) snprintf(v, n, "(empty)");
            else              snprintf(v, n, "%s", stopped_or("at IN >"));
            break;
        }
        case 14: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 15: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 16: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 17: {
            if (tp.save_busy)        snprintf(v, n, "saving...");
            else if (tp.save_id[0]) snprintf(v, n, "%s", tp.save_id);
            else                     snprintf(v, n, "%s", stopped_or("take >"));
            break;
        }
        case 18: snprintf(v, n, "%s", stopped_or("browse >")); break;
        case 19: snprintf(v, n, "%s", stopped_or("wipe >")); break;
        case 20: snprintf(v, n, "%us", (unsigned)(tp.cap / TP_RATE)); break;
    }
}

static void tape_adj(int i, int dir)
{
    float d = (float)dir;
    switch (i) {
        case 0:
            s_beats_idx = tp_clampi(s_beats_idx + dir, 0, BEAT_LADDER_N - 1);
            tape_crop_beats(BEAT_LADDER[s_beats_idx]);
            break;
        case 1: tp.clk_src = clock_source_cycle_cv_audio(tp.clk_src, dir);
                clockin_reset(&tp.ci, 1.0f); break;
        case 2: tp.manual_bpm = tp_clampf(tp.manual_bpm + d, 40, 240); break;
        case 3: { int m = tp.flt_mode + dir; if (m < 0) m = TPF_N - 1; if (m >= TPF_N) m = 0;
                  tp.flt_mode = m; } break;
        case 4: tp.cutoff = tp_clampf(tp.cutoff * (dir > 0 ? 1.12f : 0.893f), 30, 6000); break;
        case 5: tp.res01 = tp_clampf(tp.res01 + d * 0.05f, 0, 1); break;
        case 6: tp.drive = tp_clampf(tp.drive + d * 0.05f, 0, 1); break;
        case 7: tp.level = tp_clampf(tp.level + d * 0.05f, 0, 1.2f); break;
        case 8: tp.rec_src = tp.rec_src == TPS_INPUT ? TPS_TAPE : TPS_INPUT; break;
        case 9: tp.monitor = !tp.monitor; break;
        case 20: tape_set_len_sel(tp.len_sel + dir < 0 ? TP_LEN_OPTS - 1
                                  : (tp.len_sel + dir) % TP_LEN_OPTS); break;
    }
}

static int tape_action(int i)
{
    switch (i) {
        case 10: return M_TAPE_FX;       // FX -> FX sub-page
        case 11: tape_copy(); break;
        case 12: tape_cut(); break;
        case 13: tape_paste(); break;
        case 14: tape_norm(); break;
        case 15: tape_reverse(); break;
        case 16: tape_fade(); break;
        case 17: tape_save_crop(); break;
        case 18: return M_TAPE_LOAD;
        case 19: tape_clear(); break;
    }
    return 0;
}

static setup_menu_t tape_setup = {
    .items = tape_setup_items,
    .n = 21,
    .title = "Tape Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_TAPE_MAIN,
    .render = tape_val, .adjust = tape_adj, .action = tape_action,
};

static int tape_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    return setup_menu_event(&tape_setup, event);
}

// ---- FX sub-page: all five effects, reached from the Setup "FX" row --------
// Effects are clock-synced: Delay/Flanger/Tremolo divisions read tp_dly_names[].
static const setup_item_t tape_fx_items[] = {
    {"Reverb",     ST_TOGGLE}, {"Rev Mix",    ST_RANGE},
    {"Delay",      ST_TOGGLE}, {"Dly Div",    ST_RANGE},  {"Dly Fdbk",   ST_RANGE},
    {"Dly Mix",    ST_RANGE},  {"Dly Tone",   ST_RANGE},  {"Dly Ping",   ST_TOGGLE},
    {"Overdrive",  ST_TOGGLE}, {"OD Drive",   ST_RANGE},  {"OD Tone",    ST_RANGE},
    {"OD Bias",    ST_RANGE},  {"OD Level",   ST_RANGE},
    {"Flanger",    ST_TOGGLE}, {"Flg Div",    ST_RANGE},  {"Flg Depth",  ST_RANGE},
    {"Flg Fdbk",   ST_RANGE},  {"Flg Mix",    ST_RANGE},
    {"Tremolo",    ST_TOGGLE}, {"Trm Div",    ST_RANGE},  {"Trm Depth",  ST_RANGE},
    {"Trm Shape",  ST_TOGGLE}, {"Trm Stereo", ST_TOGGLE},
};

static void tape_fx_val(int i, char *v, size_t n)
{
    switch (i) {
        case 0: snprintf(v, n, "%s", reverb_mode_name(tp.rv.mode)); break;
        case 1: snprintf(v, n, "%.0f%%", tp.rv.wet * 100); break;
        case 2: snprintf(v, n, "%s", tp.dly_on ? "ON" : "OFF"); break;
        case 3: snprintf(v, n, "%s", tp_dly_names[tp.dly_div]); break;
        case 4: snprintf(v, n, "%.0f%%", tp.dly.fb * 100.0f); break;
        case 5: snprintf(v, n, "%.0f%%", tp.dly.wet * 100.0f); break;
        case 6: snprintf(v, n, "%.0f%%", tp.dly.damp * 100.0f); break;
        case 7: snprintf(v, n, "%s", tp.dly.pingpong ? "Ping-Pong" : "Stereo"); break;
        case 8: snprintf(v, n, "%s", tp.od_on ? "ON" : "OFF"); break;
        case 9: snprintf(v, n, "%.0f%%", tp.od.drive * 100); break;
        case 10: snprintf(v, n, "%.0f%%", tp.od.tone * 100); break;
        case 11: snprintf(v, n, "%+.0f%%", tp.od.bias * 100); break;
        case 12: snprintf(v, n, "%.0f%%", tp.od.level * 100); break;
        case 13: snprintf(v, n, "%s", tp.flg_on ? "ON" : "OFF"); break;
        case 14: snprintf(v, n, "%s", tp_dly_names[tp.flg_div]); break;
        case 15: snprintf(v, n, "%.0f%%", tp.flg.depth * 100); break;
        case 16: snprintf(v, n, "%+.0f%%", tp.flg.fb * 100); break;
        case 17: snprintf(v, n, "%.0f%%", tp.flg.wet * 100); break;
        case 18: snprintf(v, n, "%s", tp.trem_on ? "ON" : "OFF"); break;
        case 19: snprintf(v, n, "%s", tp_dly_names[tp.trem_div]); break;
        case 20: snprintf(v, n, "%.0f%%", tp.trem.depth * 100); break;
        case 21: snprintf(v, n, "%s", tp.trem.shape == TREM_SINE ? "Sine" :
                                      tp.trem.shape == TREM_TRI ? "Tri" : "Sqr"); break;
        case 22: snprintf(v, n, "%s", tp.trem.stereo ? "ON" : "OFF"); break;
    }
}

static void tape_fx_adj(int i, int dir)
{
    float d = (float)dir;
    switch (i) {
        case 0: { int m = tp.rv.mode + dir;
                  if (m < 0) m = RV_N_MODES - 1;
                  if (m >= RV_N_MODES) m = RV_OFF;
                  if (m != RV_OFF && !tp.rv.slab && reverb_init(&tp.rv) != ESP_OK) m = RV_OFF;
                  reverb_set_mode(&tp.rv, m); } break;
        case 1: reverb_set_mix(&tp.rv, tp_clampf(tp.rv.wet + d * 0.05f, 0, 1)); break;
        case 2: { bool on = !tp.dly_on;               // Delay on/off (lazy slab)
                   if (on && !tp.dly.bufL && fxdelay_init(&tp.dly) != ESP_OK) on = false;
                   if (on && tp.dly.wet < 0.01f) fxdelay_set_mix(&tp.dly, 0.30f);  // audible default
                   tp.dly_on = on;
                   if (!on) fxdelay_clear(&tp.dly); } break;   // drop the tail
        case 3: tp.dly_div = (tp.dly_div + dir + TP_DLY_NDIV) % TP_DLY_NDIV; break;
        case 4: if (tp.dly.bufL) fxdelay_set_feedback(&tp.dly, tp.dly.fb + d * 0.05f); break;
        case 5: if (tp.dly.bufL) fxdelay_set_mix(&tp.dly, tp.dly.wet + d * 0.05f); break;
        case 6: if (tp.dly.bufL) fxdelay_set_damp(&tp.dly, tp.dly.damp + d * 0.05f); break;
        case 7: if (tp.dly.bufL) fxdelay_set_pingpong(&tp.dly, !tp.dly.pingpong); break;
        case 8: { bool on = !tp.od_on;                // Overdrive on/off
                   if (on) {
                       if (tp.od.level < 0.01f) { tp.od.drive = 0.4f; tp.od.tone = 0.5f; tp.od.level = 0.8f; }
                       overdrive_reset(&tp.od);
                   }
                   tp.od_on = on; } break;
        case 9: tp.od.drive = tp_clampf(tp.od.drive + d * 0.05f, 0, 1); break;
        case 10: tp.od.tone  = tp_clampf(tp.od.tone  + d * 0.05f, 0, 1); break;
        case 11: tp.od.bias  = tp_clampf(tp.od.bias  + d * 0.05f, -1, 1); break;
        case 12: tp.od.level = tp_clampf(tp.od.level + d * 0.05f, 0, 1); break;
        case 13: { bool on = !tp.flg_on;               // Flanger on/off (lazy slab)
                   if (on && !tp.flg.bufL && flanger_init(&tp.flg) != ESP_OK) on = false;
                   if (on && tp.flg.wet < 0.01f) tp.flg.wet = 0.5f;   // audible default
                   tp.flg_on = on;
                   if (!on) flanger_clear(&tp.flg); } break;   // drop the tail
        case 14: tp.flg_div = (tp.flg_div + dir + TP_DLY_NDIV) % TP_DLY_NDIV; break;
        case 15: if (tp.flg.bufL) tp.flg.depth = tp_clampf(tp.flg.depth + d * 0.05f, 0, 1); break;
        case 16: if (tp.flg.bufL) tp.flg.fb    = tp_clampf(tp.flg.fb    + d * 0.05f, -0.95f, 0.95f); break;
        case 17: if (tp.flg.bufL) tp.flg.wet   = tp_clampf(tp.flg.wet   + d * 0.05f, 0, 1); break;
        case 18: { bool on = !tp.trem_on;              // Tremolo on/off
                   if (on && tp.trem.depth < 0.01f) tp.trem.depth = 0.5f;
                   tp.trem_on = on; } break;
        case 19: tp.trem_div = (tp.trem_div + dir + TP_DLY_NDIV) % TP_DLY_NDIV; break;
        case 20: tp.trem.depth = tp_clampf(tp.trem.depth + d * 0.05f, 0, 1); break;
        case 21: tp.trem.shape = (tp.trem.shape + 1) % 3; break;
        case 22: tp.trem.stereo = !tp.trem.stereo; break;
    }
}

static setup_menu_t tape_fx = {
    .items = tape_fx_items,
    .n = 23,
    .title = "Tape FX",
    .aff_label = "Setup", .aff_target = M_TAPE_SETUP,
    .live_target = M_TAPE_MAIN,
    .render = tape_fx_val, .adjust = tape_fx_adj, .action = NULL,
};

static int tape_fx_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    return setup_menu_event(&tape_fx, event);
}

// ---- Load Sample (shared browser) ----------------------------------------------
static int tape_load_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        // plain browse (all folders reachable). NOT forced into usr/TAPE — it
        // starts empty. TAPE is still a folder row + web destination.
        sample_browser_enter(true, "Load to Tape", "");
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1) { tape_load(sample_browser_selected()); return M_TAPE_MAIN; }
    if (r == 2) return M_TAPE_SETUP;
    return 0;
}

// ---- wiring ---------------------------------------------------------------------
static void tape_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_TAPE_MAIN);  menusys_item_set_default_cb(_ms, M_TAPE_MAIN, tape_main_handler);
    menusys_new_item(_ms, M_TAPE_SETUP); menusys_item_set_default_cb(_ms, M_TAPE_SETUP, tape_setup_handler);
    menusys_new_item(_ms, M_TAPE_LOAD);  menusys_item_set_default_cb(_ms, M_TAPE_LOAD, tape_load_handler);
    menusys_new_item(_ms, M_TAPE_FX);    menusys_item_set_default_cb(_ms, M_TAPE_FX, tape_fx_handler);
}

static int tape_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        char s[64];
        snprintf(s, sizeof(s), "Tape: %s %.1fs/%us", tp.recording ? "REC" : tp.playing ? "PLAY" : "stop",
                 (float)tp.pos / TP_RATE, (unsigned)(tp.cap / TP_RATE));
        _fg = TFT_LIGHTGREY;
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const tape_main_items[] = { "Tape", "Setup" };
static const int tape_main_targets[] = { M_TAPE_MAIN, M_TAPE_SETUP };

const machine_ui_t tape_menu_ui = {
    .main_items = tape_main_items,
    .main_targets = tape_main_targets,
    .n_main = 2,
    .register_pages = tape_register_pages,
    .main_event = tape_main_event,
    .boot_target = M_TAPE_MAIN,
};
