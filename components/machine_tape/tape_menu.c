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
static int s_last_state = -1;         // last transport state for the border
static unsigned s_sig_head = 0, s_sig_crop = 0;
static uint32_t s_wave_len = 0;       // len at last wave draw (record growth)

// view spans the RECORDED length (zoom to content), not the full capacity — a
// short take fills the bar instead of being a sliver on a 30 s tape.
static uint32_t tape_view_span(void) { return tp.len ? tp.len : tp.cap; }
static int frame_x(uint32_t fr)
{
    uint32_t span = tape_view_span();
    if (span == 0) return W_X;
    if (fr > span) fr = span;
    return W_X + (int)((uint64_t)fr * W_W / span);
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
            long fr = (long)((uint64_t)(x - W_X) * tape_view_span() / W_W);
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
    // column -> frame (via the zoomed view span) -> cap-binned peak index, so the
    // waveform lines up with the crop/playhead (peaks are binned over tp.cap).
    long fr = (long)((uint64_t)(x - W_X) * tape_view_span() / W_W);
    int pi = tp.cap ? (int)((uint64_t)fr * TP_PEAKS / tp.cap) : 0;
    if (pi >= 0 && pi < TP_PEAKS && tp.peaks[pi]) {
        int ph = tp.peaks[pi] * (h / 2 - 2) / 255;
        if (ph < 1) ph = 1;
        bool inside = fr >= (long)ein && fr < (long)eout;
        TFT_drawLine(x, cy - ph, x, cy + ph, inside ? WF_LIT : WF_DIM);
    } else {
        TFT_drawPixel(x, cy, (color_t){45, 45, 52}, 1);   // baseline
    }
    if (x == xi || x == xo) TFT_drawLine(x, y0, x, y0 + h, CROP_COL);
}

// transport-state border colour: REC red, PLAY green, STOP grey
static color_t tape_state_col(void)
{
    if (tp.recording) return REC_COL;
    if (tp.playing)   return (color_t){40, 200, 90};
    return (color_t){70, 78, 98};
}
// bold state-coloured outline around the transport bar (cheap; redrawn on state
// change without repainting the whole waveform)
static void draw_state_border(void)
{
    int y0 = w_y(), h = w_h();
    color_t bc = tape_state_col();
    TFT_drawRect(W_X - 3, y0 - 2, W_W + 6, h + 4, bc);
    TFT_drawRect(W_X - 2, y0 - 1, W_W + 4, h + 2, bc);
}

static void draw_wave(void)
{
    int y0 = w_y(), h = w_h();
    _bg = TFT_BLACK; TFT_fillRect(W_X - 2, y0, W_W + 4, h, _bg);
    draw_state_border();
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

// footer nav hints + a TRANSPORT hint that tracks state, so re-recording is
// discoverable: from a stopped take TR2 cuts a fresh take, and a TR2 long-hold
// erases + arms from any state (punch mode).
static void draw_footer(void)
{
    TFT_setFont(DEF_SMALL_FONT, NULL);
    _bg = TFT_BLACK;
    TFT_fillRect(0, _height - TFT_getfontheight() - 2, _width, TFT_getfontheight() + 2, _bg);
    bool punch = (tp.rec_mode == TPR_PUNCH);
    const char *tr;
    if (tp.recording)    tr = "TR1/TR2:stop";
    else if (tp.playing) tr = punch ? "TR1:stop  TR2:overdub  hold-TR2:erase"
                                    : "TR1:stop  TR2:overdub";
    else if (tp.len)     tr = "TR1:play  TR2:new take";   // tap saves the old take first
    else                 tr = "TR1:play  TR2:record";
    char line[96];
    snprintf(line, sizeof(line), "turn:move  hold:setup    %s", tr);
    _fg = (color_t){90, 90, 90};
    TFT_print(line, 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static unsigned head_sig(void)
{
    // position at 0.1 s resolution so the readout visibly counts while rolling
    return (tp.playing ? 1u : 0u) + (tp.recording ? 2u : 0u)
         + (unsigned)tp.disp_bpm * 8u + ((unsigned)((float)tp.pos * 10.0f / TP_RATE) << 8);
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
    tp.cropped = true;                  // user shaped the crop -> auto-save marks it (TCR_)
}

static int tape_main_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: main_full_redraw(); break;
        case EV_FWD: nudge(+1); draw_wave(); draw_readout(); s_sig_crop = crop_sig(); break;
        case EV_BWD: nudge(-1); draw_wave(); draw_readout(); s_sig_crop = crop_sig(); break;
        case EV_SHORT_PRESS:
            s_sel = (s_sel + 1) % 3;   // IN / OUT / WIN (encoder = focus/select, not transport)
            draw_readout(); s_sig_crop = crop_sig();
            break;
        case EV_LONG_PRESS: return M_TAPE_SETUP;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST: {
            tape_autosave_kick();                        // spawn any auto-save the audio task requested
            if (tp.recording && tp.len != s_wave_len) {  // live record growth (or erase reset)
                tape_rebuild_peaks(false);
                draw_wave();
            }
            unsigned hs = head_sig();
            if (hs != s_sig_head) {
                draw_header();
                int st = tp.recording ? 2 : tp.playing ? 1 : 0;
                if (st != s_last_state) { draw_state_border(); draw_footer(); s_last_state = st; }
                s_sig_head = hs;
            }
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

// Tape/transport params + edit actions live here; the FX rack lives on three
// slot rows (FX1/FX2 generic + FX3 reverb), each opening the shared dynamic
// per-slot sub-page (Effect select + its params). Keeps this list readable.
static const setup_item_t tape_setup_items[] = {
    {"Crop Beats", ST_RANGE},  {"Clock Src",  ST_TOGGLE}, {"Manual BPM", ST_RANGE},
    {"Filter",     ST_TOGGLE}, {"Cutoff",     ST_RANGE},  {"Reso",       ST_RANGE},
    {"Drive",      ST_RANGE},  {"Level",      ST_RANGE},  {"Rec Source", ST_TOGGLE},
    {"Monitor",    ST_TOGGLE},
    {"FX1",        ST_ACTION}, {"FX2",        ST_ACTION}, {"FX3 Reverb", ST_ACTION},
    {"Copy",       ST_ACTION}, {"Cut",        ST_ACTION}, {"Paste",      ST_ACTION},
    {"Normalize",  ST_ACTION}, {"Reverse",    ST_ACTION}, {"Fade Edges", ST_ACTION},
    {"Save Crop",  ST_ACTION}, {"Load Sample", ST_ACTION}, {"Clear Tape", ST_ACTION},
    {"Tape Len",   ST_TOGGLE}, {"Rec Mode",    ST_TOGGLE}, {"Play Mode",   ST_TOGGLE},
};

// FX rack slot editor state (shared fxrack machinery drives the rows)
static int s_cur_slot = 0;            // slot the FX sub-page is editing
static int s_setup_return = -1;       // Setup row to restore on return from the sub-page

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
        case 10: snprintf(v, n, "%s >", fxrack_slot_name(&tp_rk, 0)); break;
        case 11: snprintf(v, n, "%s >", fxrack_slot_name(&tp_rk, 1)); break;
        case 12: snprintf(v, n, "%s >", fxrack_slot_name(&tp_rk, 2)); break;
        case 13: {
            if (tp.clip_len) snprintf(v, n, "%.2fs held", (float)tp.clip_len / TP_RATE);
            else             snprintf(v, n, "%s", stopped_or("copy >"));
            break;
        }
        case 14: snprintf(v, n, "%s", stopped_or("cut >")); break;
        case 15: {
            if (!tp.clip_len) snprintf(v, n, "(empty)");
            else              snprintf(v, n, "%s", stopped_or("at IN >"));
            break;
        }
        case 16: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 17: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 18: snprintf(v, n, "%s", stopped_or("crop >")); break;
        case 19: {
            if (tp.save_busy)        snprintf(v, n, "saving...");
            else if (tp.save_id[0]) snprintf(v, n, "%s", tp.save_id);
            else                     snprintf(v, n, "%s", stopped_or("take >"));
            break;
        }
        case 20: snprintf(v, n, "%s", stopped_or("browse >")); break;
        case 21: snprintf(v, n, "%s", stopped_or("wipe >")); break;
        case 22: snprintf(v, n, "%us", (unsigned)(tp.cap / TP_RATE)); break;
        case 23: snprintf(v, n, "%s", tp.rec_mode == TPR_MOMENTARY ? "momentary" : "punch"); break;
        case 24: snprintf(v, n, "%s", tp.play_oneshot ? "one-shot" : "loop"); break;
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
        case 22: tape_set_len_sel(tp.len_sel + dir < 0 ? TP_LEN_OPTS - 1
                                  : (tp.len_sel + dir) % TP_LEN_OPTS); break;
        case 23: tp.rec_mode = tp.rec_mode == TPR_PUNCH ? TPR_MOMENTARY : TPR_PUNCH; break;
        case 24: tp.play_oneshot = !tp.play_oneshot; break;
    }
}

static int tape_action(int i)
{
    switch (i) {
        case 10: s_setup_return = 10; s_cur_slot = 0; return M_TAPE_FX;   // FX1
        case 11: s_setup_return = 11; s_cur_slot = 1; return M_TAPE_FX;   // FX2
        case 12: s_setup_return = 12; s_cur_slot = 2; return M_TAPE_FX;   // FX3 reverb
        case 13: tape_copy(); break;
        case 14: tape_cut(); break;
        case 15: tape_paste(); break;
        case 16: tape_norm(); break;
        case 17: tape_reverse(); break;
        case 18: tape_fade(); break;
        case 19: tape_save_crop(); break;
        case 20: return M_TAPE_LOAD;
        case 21: tape_clear(); break;
    }
    return 0;
}

static setup_menu_t tape_setup = {
    .items = tape_setup_items,
    .n = 25,
    .title = "Tape Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_TAPE_MAIN,
    .render = tape_val, .adjust = tape_adj, .action = tape_action,
};

static int tape_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU && s_setup_return >= 0) {   // return from a slot sub-page
        int p = s_setup_return; s_setup_return = -1;
        setup_menu_enter_at(&tape_setup, p);
        return 0;
    }
    return setup_menu_event(&tape_setup, event);
}

// ---- FX slot editor: one slot at a time, rows driven by the shared fxrack ----
// Reached from the FX1/FX2/FX3 Setup rows; the rate effects (delay/flanger/
// tremolo) carry a Sync toggle + Div, and Tape feeds the grid BPM so they lock.
static setup_menu_t tape_fx;             // defined below
static setup_item_t s_tfx_items[FXRACK_MAXROWS];
static int8_t s_tfx_param[FXRACK_MAXROWS];
static int s_tfx_n;

static void tfx_rebuild(void)
{
    s_tfx_n = fxrack_menu_rows(&tp_rk, s_cur_slot, s_tfx_items, s_tfx_param);
    tape_fx.n = s_tfx_n;
    tape_fx.title = s_cur_slot == 0 ? "Tape FX1" : s_cur_slot == 1 ? "Tape FX2" : "Tape FX3";
    if (tape_fx.sel >= s_tfx_n) tape_fx.sel = s_tfx_n - 1;
    if (tape_fx.sel < 0) tape_fx.sel = 0;
}

static void tape_fx_val(int i, char *v, size_t n)
{
    if (i < 0 || i >= s_tfx_n) { v[0] = 0; return; }
    fxrack_menu_val(&tp_rk, s_cur_slot, s_tfx_param[i], v, n);
}

static void tape_fx_adj(int i, int dir)
{
    if (i < 0 || i >= s_tfx_n) return;
    int p = s_tfx_param[i];
    fxrack_menu_adj(&tp_rk, s_cur_slot, p, dir);
    if (p < 0) tfx_rebuild();            // effect changed -> param rows changed
}

static setup_menu_t tape_fx = {
    .items = s_tfx_items,
    .n = 0,                              // set by tfx_rebuild()
    .title = "Tape FX",                  // overwritten per-slot by tfx_rebuild()
    .aff_label = "Setup", .aff_target = M_TAPE_SETUP,
    .live_target = M_TAPE_MAIN,
    .render = tape_fx_val, .adjust = tape_fx_adj, .action = NULL,
};

static int tape_fx_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) tfx_rebuild();   // dynamic row set for s_cur_slot
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
    tape_autosave_kick();   // machine-ambient: spawn a requested auto-save from any Tape page
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
