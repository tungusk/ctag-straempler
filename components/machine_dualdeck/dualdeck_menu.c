// Dual-deck UI — Live (side-by-side panels + crossfade meter + big tempo),
// Setup (house row grammar), Load browser (dated, newest first) for the
// focused deck. All drawing change-driven (house discipline).
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "setup_menu.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "audio.h"
#include "sample_ram.h"
#include "sample_browser.h"
#include "beatlisten.h"
#include "menu_config.h"
#include "dualdeck_priv.h"

static const color_t COL_ARM  = {230, 170, 0};
static const color_t WF_GREY  = {125, 125, 135};


// ---- Live -------------------------------------------------------------------
// TWO DISTINCT LAYOUTS, picked in Setup (Arlo: "not two geometries — distinct
// layouts"). They are separate renderers on purpose; each is designed for its
// own shape rather than one drawing routine stretched over both.
//
//   V (stacked, default) — TWO SINGLE DECKS, one above the other. Black
//     background, the deck's real transport bar full width (fat state border,
//     grey waveform, white playhead, the box SHRINKING to the loop window),
//     A's text left-justified above its bar, B's right-justified below its own,
//     and the crossfader as a hairline between them — the layout mirrors around
//     the fader, which is where the mix physically happens.
//   H (side by side) — the panel look: two tiles, each with its own framed
//     waveform strip. sampler3 earns this arrangement because its pads and
//     knobs are physically side by side; kept here because taste is taste.
//
// Shared: the tempo readout, the fader, the state->colour mapping. Nothing else.

static int  s_last_focus = -1;
static int  s_last_barx[2] = {-1, -1};
static int  s_bar_state[2] = {-1, -1};
static int  s_wf_drawn[2] = {-1, -1};
static int  s_loop_key[2] = {-2, -2};
static char s_last_hdr[2][96];
static int  s_last_dbpm = -1;
static int  s_last_xfx = -1;
static int  s_last_layout = -1;
static int  s_last_fx = -1;

// transport state (shared): 1 = playing, 2 = armed for the bar, 0 = stopped.
// LOOPING is NOT a state colour — the box shrinks to the window instead, and
// that shrink IS the signal (the deck's convention).
static int tbar_state(int i){
    dd_deck_t *v = &dd.d[i];
    if (v->arm_start || v->arm_stop) return 2;
    return v->playing ? 1 : 0;
}
static color_t tbar_bg(int i){
    switch (tbar_state(i)){
        case 2:  return COL_ARM;                   // armed: fires on the bar
        case 1:  return (color_t){25, 120, 50};    // green: playing
        default: return (color_t){30, 60, 140};    // blue: stopped
    }
}
static int loop_key(int i){
    dd_deck_t *v = &dd.d[i];
    return v->loop_active ? (int)(v->ui_lstart >> 9) * 31 + (int)(v->ui_llen >> 9) : -1;
}

// status string both layouts print ("PLAY  124.0  1:03/5:12"). REVERSED for
// deck 2 in the stacked layout (Arlo: "for symmetry the info text in deck 2 can
// be reverse order: time, tempo, play/stop") — it is right-justified there, so
// reversing puts the state word on the outside edge, mirroring deck 1.
static void deck_info_str(int i, char *out, size_t n, bool rev){
    dd_deck_t *v = &dd.d[i];
    char st[20];
    int stt = tbar_state(i);
    if (v->resync_armed){              // both trigs held: the beat lands on release
        snprintf(st, sizeof(st), "RESYNC");
    } else if (v->loop_active){
        char b[8];
        dd_fmt_beats(v->loop_beats, b, sizeof(b));
        snprintf(st, sizeof(st), "LOOP %s", b);
    } else snprintf(st, sizeof(st), "%s", stt == 1 ? "PLAY" : stt == 2 ? "ARM" : "STOP");
    // NO LIVE TIME (Arlo: "the visuals are also choppy... feel free to drop the
    // live time readouts"). The elapsed clock changed every second, and since the
    // whole header repaints when its string changes, that was a full-width repaint
    // of BOTH decks' name+info rows once a second — for a number nobody reads
    // mid-set. The playhead already says where you are, continuously and cheaply.
    // What is left changes only on a STATE change, so the header now sits still.
    if (v->file_frames){
        int tt = (int)(v->file_frames / DD_RATE);
        if (v->track_bpm > 0){
            if (rev) snprintf(out, n, "%d:%02d  %.1f  %s", tt / 60, tt % 60, v->track_bpm, st);
            else     snprintf(out, n, "%s  %.1f  %d:%02d", st, v->track_bpm, tt / 60, tt % 60);
        } else {
            if (rev) snprintf(out, n, "no grid  %s", st);
            else     snprintf(out, n, "%s  no grid", st);
        }
    } else snprintf(out, n, "%s", st);
}

// display EMA of the shared external tempo (deck pattern: raw per-pulse bpm
// dances ~+/-0.35 from block-quantized edges; snap through real changes)
static float ext_bpm_disp(void){
    static float ema = 0;
    static uint32_t last_tk = 0;
    float x = clockin_beat_bpm(&dd.ci);
    if (x <= 0) { ema = 0; return 0; }
    float d = x - ema;
    if (ema <= 0 || d > 2.0f || d < -2.0f) { ema = x; return ema; }
    uint32_t tk = xTaskGetTickCount();
    if (tk - last_tk >= pdMS_TO_TICKS(250)) { last_tk = tk; ema += 0.08f * d; }
    return ema;
}

static void draw_big_bpm(void){
    float bpm = ext_bpm_disp();
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
    _fg = dd.ci.clk.locked ? (color_t){40, 200, 90} : TFT_WHITE;
    TFT_print(s, _width - TFT_getStringWidth(s) - 8, 4);
    cfont = f;
}

// =====================================================================
//  V LAYOUT — two single decks, stacked. Black canvas, the real deck bar.
// =====================================================================
#define V_TB_X   8
#define V_TB_W   (_width - 16)
#define V_TB_H   36
#define V_TB_BW  3                       // fat state border (deck idiom)
#define V_NAME_H 30                      // the big track-name row (DEJAVU24)
#define V_A_NAME 34
#define V_A_INFO (V_A_NAME + V_NAME_H)
#define V_BAR_A  (V_A_INFO + TFT_getfontheight() + 5)
// the fader is a HAIRLINE between the decks; deck 2 comes right back up to meet
// it (Arlo: "theres extra padding below the crossfader, tighten that up")
#define V_XF_Y   (V_BAR_A + V_TB_H + 8)
#define V_BAR_B  (V_XF_Y + 8)
#define V_B_INFO (V_BAR_B + V_TB_H + 5)
#define V_B_NAME (V_B_INFO + TFT_getfontheight() + 3)

static int v_bar_y(int i){ return i == 0 ? V_BAR_A : V_BAR_B; }

// black canvas + grey waveform, clipped to [x, x+w) so a playhead erase repaints it
static void v_paint_slice(int i, int x, int w){
    dd_deck_t *v = &dd.d[i];
    int by = v_bar_y(i);
    TFT_fillRect(x, by + V_TB_BW, w, V_TB_H - 2 * V_TB_BW, (color_t){0, 0, 0});
    if (v->wf_state == 2){
        int wx = V_TB_X + V_TB_BW + 1, ww = V_TB_W - 2 * V_TB_BW - 2;
        int wy = by + V_TB_BW + 1, wh = V_TB_H - 2 * V_TB_BW - 2;
        for (int c = 0; c < ww - 1; c += 2){
            int px = wx + c;
            if (px + 2 <= x || px >= x + w) continue;
            float a = sqrtf((float)v->wf[(c * DD_WF_W) / ww] / 255.0f);
            int h = (int)(a * (float)wh);
            if (h < 2) h = 2;
            TFT_fillRect(px, wy + (wh - h) / 2, 2, h, WF_GREY);
        }
    }
    if (i == dd.focus){            // SELECTED track: thin white centre line
        int cwx = V_TB_X + V_TB_BW, cww = V_TB_W - 2 * V_TB_BW;
        int lx = x > cwx ? x : cwx;
        int lr = (x + w) < (cwx + cww) ? (x + w) : (cwx + cww);
        if (lr > lx) TFT_fillRect(lx, by + V_TB_H / 2, lr - lx, 1, (color_t){245, 245, 245});
    }
}

// THE BOX IS THE LOOP: the fat border stops framing the whole track and shrinks
// to frame the WINDOW, keeping the transport colour.
static void v_loop_box(int i, int sx, int sw){
    dd_deck_t *v = &dd.d[i];
    if (!v->loop_active || !v->file_frames || !v->ui_llen) return;
    int by = v_bar_y(i);
    int bx = V_TB_X + V_TB_BW + 1, bw = V_TB_W - 2 * V_TB_BW - 6;
    int x0 = bx + (int)((uint64_t)v->ui_lstart * bw / v->file_frames);
    int x1 = bx + (int)((uint64_t)(v->ui_lstart + v->ui_llen) * bw / v->file_frames);
    if (x1 < x0 + 2 * V_TB_BW) x1 = x0 + 2 * V_TB_BW;   // stay legible when tiny
    color_t pk = tbar_bg(i);
    struct { int x, w, y, h; } seg[4] = {
        { x0, x1 - x0, by, V_TB_BW },
        { x0, x1 - x0, by + V_TB_H - V_TB_BW, V_TB_BW },
        { x0, V_TB_BW, by, V_TB_H },
        { x1 - V_TB_BW, V_TB_BW, by, V_TB_H },
    };
    for (int k = 0; k < 4; k++){
        int a = seg[k].x > sx ? seg[k].x : sx;
        int b = (seg[k].x + seg[k].w) < (sx + sw) ? (seg[k].x + seg[k].w) : (sx + sw);
        if (b > a) TFT_fillRect(a, seg[k].y, b - a, seg[k].h, pk);
    }
}

static void v_bar_frame(int i){
    dd_deck_t *v = &dd.d[i];
    int by = v_bar_y(i);
    if (v->loop_active){
        TFT_fillRect(V_TB_X, by, V_TB_W, V_TB_H, (color_t){0, 0, 0});   // no full border
        v_paint_slice(i, V_TB_X, V_TB_W);
    } else {
        TFT_fillRect(V_TB_X, by, V_TB_W, V_TB_H, tbar_bg(i));           // border colour
        v_paint_slice(i, V_TB_X + V_TB_BW, V_TB_W - 2 * V_TB_BW);       // black canvas
    }
    _bg = TFT_BLACK;            // shared global: leaking the bar colour tints text
    s_last_barx[i] = -1;
    s_bar_state[i] = tbar_state(i);
    s_wf_drawn[i] = v->wf_state;
}

static void v_bar(int i){
    dd_deck_t *v = &dd.d[i];
    if (tbar_state(i) != s_bar_state[i] ||
        (v->wf_state == 2) != (s_wf_drawn[i] == 2) ||
        loop_key(i) != s_loop_key[i]){
        v_bar_frame(i);
        v_loop_box(i, V_TB_X, V_TB_W);
        s_loop_key[i] = loop_key(i);
    }
    if (!v->file_frames) return;
    int by = v_bar_y(i);
    int x = V_TB_X + V_TB_BW + 1 +
            (int)((uint64_t)v->ui_fpos * (V_TB_W - 2 * V_TB_BW - 6) / v->file_frames);
    if (x == s_last_barx[i]) return;
    if (s_last_barx[i] > 0){
        v_paint_slice(i, s_last_barx[i], 5);
        v_loop_box(i, s_last_barx[i], 5);
    }
    TFT_fillRect(x, by + V_TB_BW, 5, V_TB_H - 2 * V_TB_BW, (color_t){245, 245, 245});
    s_last_barx[i] = x;
}

// A: name/info LEFT above its bar. B: RIGHT below its own — mirrored on the fader.
static void v_hdr(int i, bool full){
    dd_deck_t *v = &dd.d[i];
    bool focus = (i == dd.focus);
    bool right = (i == 1);              // deck 2 mirrors deck 1 around the fader
    char nm[32], info[48], sig[96];
    // the info line takes the OPPOSITE justification from the number+name, so
    // each deck spans the screen: its number on one edge, its state on the other
    bool inf_right = !right;
    deck_info_str(i, info, sizeof(info), inf_right);
    // auto-BPM in progress on this (stopped) deck — tells you WHY it can't loop
    // yet, right where "no grid" would otherwise sit. Progress in the sig repaints.
    if (dd.an_running && dd.an_deck == i)
        snprintf(info, sizeof(info), "analyzing %d%%", dd.an_progress);
    const char *tn = v->track[0] ? v->track : "(empty)";
    snprintf(nm, sizeof(nm), "%.10s", tn);
    snprintf(sig, sizeof(sig), "%d|%s|%s", focus ? 1 : 0, nm, info);
    if (!full && strcmp(sig, s_last_hdr[i]) == 0) return;
    strlcpy(s_last_hdr[i], sig, sizeof(s_last_hdr[i]));

    int ny = (i == 0) ? V_A_NAME : V_B_NAME;
    int iy = (i == 0) ? V_A_INFO : V_B_INFO;
    _bg = TFT_BLACK;
    TFT_fillRect(0, ny - 2, _width, V_NAME_H, _bg);

    // THE NUMBER IS THE SELECTION (Arlo): the focused deck wears its number as
    // BLACK ON A WHITE SQUARE — unmissable at a glance, which is what focus has
    // to be when the trigs and both knobs address it. The unfocused deck gets a
    // dim plate, so the geometry never shifts.
    Font f = cfont;
    TFT_setFont(DEJAVU24_FONT, NULL);
    int nfh = TFT_getfontheight();
    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    int numw = TFT_getStringWidth(num);
    int boxw = numw + 12, boxh = nfh + 4;
    int namew = TFT_getStringWidth(nm);
    // deck 1 leads with its number, deck 2 TRAILS with it — the numbers sit on
    // the OUTER edges ("move the 2 to the right, following the track name")
    int boxx = right ? (V_TB_X + V_TB_W - boxw) : V_TB_X;
    int namex = right ? (V_TB_X + V_TB_W - boxw - 10 - namew) : (V_TB_X + boxw + 10);

    // FOCUSED: black on a filled WHITE plate. UNFOCUSED: blue numeral in a blue
    // OUTLINE on black (Arlo) — same square either way, so the two decks read as
    // the same badge in two states and nothing shifts when focus moves.
    if (focus){
        TFT_fillRect(boxx, ny - 2, boxw, boxh, TFT_WHITE);
        _bg = TFT_WHITE; _fg = TFT_BLACK;
    } else {
        color_t bl = (color_t){70, 110, 190};
        _fg = bl;
        TFT_drawRect(boxx, ny - 2, boxw, boxh, _fg);
        _bg = TFT_BLACK; _fg = bl;
    }
    TFT_print(num, boxx + (boxw - numw) / 2, ny);
    _bg = TFT_BLACK;
    _fg = focus ? TFT_WHITE : (color_t){95, 105, 135};
    TFT_print(nm, namex, ny);
    cfont = f;

    int fh = TFT_getfontheight();
    TFT_fillRect(0, iy - 1, _width, fh + 2, _bg);
    _fg = (color_t){120, 130, 160};
    TFT_print(info, inf_right ? V_TB_X + V_TB_W - TFT_getStringWidth(info) : V_TB_X, iy);
}

// =====================================================================
//  H LAYOUT — side-by-side panels (the original), with taller tiles.
// =====================================================================
static const color_t PANEL_BG = {5, 9, 28};
#define H_PANEL_Y (TFT_getfontheight() + 30)
#define H_PANEL_H 118
#define H_BAR_H   44
#define H_XF_Y    (H_PANEL_Y + H_PANEL_H + 12)

static int h_px(int i){ return (i == 0) ? 4 : _width / 2 + 2; }
static int h_pw(void){ return _width / 2 - 6; }

static void h_bar_slice(int i, int sx, int sw){
    dd_deck_t *v = &dd.d[i];
    int bx = h_px(i) + 3, bw = h_pw() - 6;
    int by = H_PANEL_Y + H_PANEL_H - H_BAR_H - 4;
    if (sx < bx) { sw -= bx - sx; sx = bx; }
    if (sx + sw > bx + bw) sw = bx + bw - sx;
    if (sw <= 0) return;
    TFT_fillRect(sx, by, sw, H_BAR_H, (color_t){0, 0, 0});
    if (v->wf_state == 2){
        int wy = by + 2, wh = H_BAR_H - 4;
        for (int c = 0; c < bw - 1; c += 2){
            int x = bx + c;
            if (x + 2 <= sx || x >= sx + sw) continue;
            float a = sqrtf((float)v->wf[(c * DD_WF_W) / bw] / 255.0f);
            int h = (int)(a * (float)wh);
            if (h < 2) h = 2;
            TFT_fillRect(x, wy + (wh - h) / 2, 2, h, WF_GREY);
        }
    }
    if (i == dd.focus){            // SELECTED track: thin white centre line
        int lx = sx > bx ? sx : bx;
        int lr = (sx + sw) < (bx + bw) ? (sx + sw) : (bx + bw);
        if (lr > lx) TFT_fillRect(lx, by + H_BAR_H / 2, lr - lx, 1, (color_t){245, 245, 245});
    }
    // loop window: rails top and bottom + end posts, in the transport colour
    if (v->loop_active && v->ui_llen && v->file_frames){
        color_t lc = tbar_bg(i);
        int x0 = bx + (int)((uint64_t)v->ui_lstart * (bw - 3) / v->file_frames);
        int x1 = bx + (int)((uint64_t)(v->ui_lstart + v->ui_llen) * (bw - 3) / v->file_frames);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > bx + bw - 1) x1 = bx + bw - 1;
        int cx = (x0 < sx) ? sx : x0;
        int cw = ((x1 < sx + sw) ? x1 : sx + sw) - cx;
        if (cw > 0){
            TFT_fillRect(cx, by, cw, 2, lc);
            TFT_fillRect(cx, by + H_BAR_H - 2, cw, 2, lc);
        }
        if (x0 >= sx && x0 < sx + sw) TFT_fillRect(x0, by, 1, H_BAR_H, lc);
        if (x1 >= sx && x1 < sx + sw) TFT_fillRect(x1, by, 1, H_BAR_H, lc);
    }
}

static void h_playhead(int i){
    dd_deck_t *v = &dd.d[i];
    int bx = h_px(i) + 3, bw = h_pw() - 6;
    int by = H_PANEL_Y + H_PANEL_H - H_BAR_H - 4;
    int ph = -1;
    if (v->file_frames && v->track[0])
        ph = (int)((uint64_t)(v->ui_fpos < v->file_frames ? v->ui_fpos : v->file_frames)
                   * (bw - 3) / v->file_frames);
    if (ph == s_last_barx[i]) return;
    if (s_last_barx[i] >= 0) h_bar_slice(i, bx + s_last_barx[i], 3);
    if (ph >= 0) TFT_fillRect(bx + ph, by, 3, H_BAR_H, tbar_bg(i));
    s_last_barx[i] = ph;
}

static void h_panel(int i, bool full){
    dd_deck_t *v = &dd.d[i];
    int pw = h_pw(), px = h_px(i);
    bool focus = (i == dd.focus);
    char nm[32], info[48], sig[96];
    snprintf(nm, sizeof(nm), "%c %.11s", i == 0 ? 'A' : 'B',
             v->track[0] ? v->track : "(empty)");
    deck_info_str(i, info, sizeof(info), false);   // panels: both read normally
    snprintf(sig, sizeof(sig), "%d|%s|%s|%d|%d", focus ? 1 : 0, nm, info,
             v->wf_state == 2 ? 1 : 0, loop_key(i));
    bool repaint = full || strcmp(sig, s_last_hdr[i]) != 0;
    if (!repaint){ h_playhead(i); return; }
    strlcpy(s_last_hdr[i], sig, sizeof(s_last_hdr[i]));

    _bg = PANEL_BG;
    TFT_fillRect(px, H_PANEL_Y, pw, H_PANEL_H, _bg);
    _fg = focus ? TFT_WHITE : (color_t){40, 60, 110};
    TFT_drawRect(px, H_PANEL_Y, pw, H_PANEL_H, _fg);
    if (focus) TFT_drawRect(px + 1, H_PANEL_Y + 1, pw - 2, H_PANEL_H - 2, _fg);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print(nm, px + 6, H_PANEL_Y + 6);
    _fg = tbar_bg(i);
    TFT_print(info, px + 6, H_PANEL_Y + 6 + fh + 4);
    _bg = TFT_BLACK;
    h_bar_slice(i, px + 3, pw - 6);
    s_last_barx[i] = -1;
    h_playhead(i);
}

// =====================================================================
//  Shared: the crossfader — a thin RULE with a scrubber crossing it (Arlo)
// =====================================================================
#define XF_RULE  1      // a hairline (Arlo)
#define XF_OVER  5      // the scrubber crosses it by a few px
static int xf_y(void){ return dd.layout == DD_LAY_V ? V_XF_Y : H_XF_Y; }

static void draw_xfade(bool full){
    int y = xf_y(), x0 = 8, w = _width - 16;
    int xfx = (int)(dd.xf * (float)(w - 5));
    if (!full && xfx == s_last_xfx) return;
    int sy = y - XF_OVER, sh = XF_RULE + 2 * XF_OVER;
    if (!full && s_last_xfx >= 0)
        TFT_fillRect(x0 + s_last_xfx, sy, 5, sh, (color_t){0, 0, 0});   // erase old
    s_last_xfx = xfx;
    TFT_fillRect(x0, y, w, XF_RULE, (color_t){40, 60, 110});            // the rule
    TFT_fillRect(x0 + w / 2, sy + 1, 1, sh - 2, (color_t){40, 60, 110});// centre detent
    color_t sc = dd.auto_active ? COL_ARM : (color_t){170, 200, 240};
    TFT_fillRect(x0 + xfx, sy, 5, sh, sc);                              // the scrubber
    _bg = TFT_BLACK;
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("DoubleDecker", 6, 4);
    s_last_dbpm = -1;
    s_last_focus = -1;
    s_last_xfx = -1;
    s_last_fx = -1;
    s_last_layout = dd.layout;
    for (int i = 0; i < 2; i++){
        s_bar_state[i] = -1;
        s_loop_key[i] = -2;
        s_last_barx[i] = -1;
        s_last_hdr[i][0] = 0;
    }
    draw_big_bpm();
    for (int i = 0; i < 2; i++){
        if (dd.layout == DD_LAY_V){ v_hdr(i, true); v_bar(i); }
        else h_panel(i, true);
    }
    s_last_focus = dd.focus;
    draw_xfade(true);
    _bg = TFT_BLACK; _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    {
        // The hint tells you what the knobs mean in THIS context — the whole point of
        // contextual mode is that it changes, so a static legend would be a lie.
        char h[128];   // sized generously: a truncating snprintf is a stack smash
        if (dd.knob_mode == DD_KNOB_CTX && dd.d[dd.focus].loop_active)
            snprintf(h, sizeof(h), "turn:focus  press:load  hold:setup   deck %d LOOP: CV6 window  CV7 %s",
                     dd.focus + 1, dd.fader_lock ? "(fader locked)" : "length");
        else
            snprintf(h, sizeof(h), "turn:focus  press:load  hold:setup   CV6 filter  CV7 fader");
        TFT_print(h, 6, _height - TFT_getfontheight() - 1);
    }
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void live_tick(void){
    draw_big_bpm();
    for (int i = 0; i < 2; i++){
        if (dd.layout == DD_LAY_V){ v_hdr(i, false); v_bar(i); }
        else h_panel(i, false);
    }
    s_last_focus = dd.focus;
    draw_xfade(false);
}

static int dd_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST: {
            if (dd.layout != s_last_layout){ live_full_redraw(); break; }
            live_tick();
            if (event == EV_TIMER_REPEATING_SLOW){
                char dbg[64];
                snprintf(dbg, sizeof(dbg), "A%c s%lu E%+d | B%c s%lu E%+d | x%d p%lu L%d",
                         dd.d[0].loop_active ? 'L' : (dd.d[0].playing ? 'P' : 's'),
                         (unsigned long)dd.d[0].dbg_starve,
                         (int)(dd.d[0].phase_err * 100),
                         dd.d[1].loop_active ? 'L' : (dd.d[1].playing ? 'P' : 's'),
                         (unsigned long)dd.d[1].dbg_starve,
                         (int)(dd.d[1].phase_err * 100),
                         (int)(dd.xf * 100),
                         (unsigned long)(dd.ci.clk.period / 44),
                         (int)dd.ci.clk.locked);
                audio_status_set_voices("doubledecker", dbg);
            }
            break;
        }
        case EV_FWD:
        case EV_BWD:
            dd.focus = 1 - dd.focus;
            live_full_redraw();      // focus lives in the frame/accent: repaint
            break;
        case EV_SHORT_PRESS:
            // no reloading a track onto a PLAYING deck (Arlo) — the press is a
            // no-op while it plays, freeing the gesture for a future use
            if (dd.d[dd.focus].playing) break;
            return M_DD_LOAD;
        case EV_LONG_PRESS:
            return M_DD_SETUP;
        default: break;
    }
    return 0;
}

// ---- Load browser: the shared two-level widget (folders -> big-name list) --
static int dd_load_handler(int it_id, int event, void *ev_data){
    if (event == EV_ENTERED_MENU){
        char t[32];
        snprintf(t, sizeof(t), "Load -> deck %d", dd.focus + 1);
        sample_browser_enter(true, t, dd.d[dd.focus].track);
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1){ dualdeck_load_track(dd.focus, (char*)sample_browser_selected()); return M_DD_LIVE; }
    if (r == 2) return M_DD_LIVE;
    return 0;
}

// ---- Setup (shared setup-menu framework) ----
// TOGGLE = small cycle-on-press option set; RANGE = press [ ] edit + turn (the
// wide Clock Src CV list + the ppb list); ACTION = press opens a sub-page.
#define DD_ROW_KNOBS 5
#define DD_ROW_FLOCK 6
#define DD_ROW_CVMAP 7                                     // opens its own page
static const setup_item_t dd_setup_items[] = {
    {"Clock Src",  ST_RANGE},    // 0  CV1..8 + AUDIO
    {"Clock",      ST_RANGE},    // 1  pulses per beat
    {"Takeover",   ST_TOGGLE},   // 2  off / cut / N beats
    {"Loop Len",   ST_TOGGLE},   // 3  quarter-beat ladder
    {"Layout",     ST_TOGGLE},   // 4  stacked / side by side
    {"Knobs",      ST_TOGGLE},   // 5  contextual / fixed  (DD_ROW_KNOBS)
    {"Fader Lock", ST_TOGGLE},   // 6  on / off            (DD_ROW_FLOCK)
    {"CV Map",     ST_ACTION},   // 7  sub-page            (DD_ROW_CVMAP)
};
#define DD_SETUP_N ((int)(sizeof(dd_setup_items) / sizeof(dd_setup_items[0])))

static void setup_value_str(int i, char *v, size_t n){
    switch(i){
        case 0: snprintf(v, n, "%s", clock_source_name(dd.clk_src)); break;
        case 1: snprintf(v, n, "%s", dd_ppb_names[dd.ppb_idx]); break;
        case 2:                          // -1 = the fader never moves itself
            if (dd.fade_beats < 0) snprintf(v, n, "off");
            else if (dd.fade_beats == 0) snprintf(v, n, "cut");
            else snprintf(v, n, "%d beats", dd.fade_beats);
            break;
        case 3: {                       // QUARTER-beats: 1/4 .. 256
            char b[8];
            dd_fmt_beats(dd.loop_len_beats, b, sizeof(b));
            snprintf(v, n, "%s beat%s", b, dd.loop_len_beats == 4 ? "" : "s");
            break;
        }
        case 4: snprintf(v, n, "%s", dd.layout == DD_LAY_V ? "stacked" : "side by side"); break;
        case DD_ROW_KNOBS:
            snprintf(v, n, "%s", dd.knob_mode == DD_KNOB_CTX ? "contextual" : "fixed");
            break;
        case DD_ROW_FLOCK:
            snprintf(v, n, "%s", dd.fader_lock ? "on" : "off");
            break;
        case DD_ROW_CVMAP:
            snprintf(v, n, "%s", dd.knob_mode == DD_KNOB_CTX ? "(fixed only)" : "...");
            break;
        default: v[0] = 0;
    }
}

static void setup_adj(int i, int dir){
    switch(i){
        case 0:
            // CV1..8 + AUDIO (the trigs are the transport); AUDIO wakes the ear
            dd.clk_src = clock_source_cycle_cv_audio(dd.clk_src, dir);
            if (dd.clk_src == CLK_SRC_AUDIO && beatlisten_get_mode() == BL_OFF) {
                beatlisten_set_mode(BL_GROOVE);
                configSetIntSetting("blisten", BL_GROOVE);
            }
            break;
        case 1:
            dd.ppb_idx += dir;
            if (dd.ppb_idx < 0) dd.ppb_idx = 0;
            if (dd.ppb_idx > 5) dd.ppb_idx = 5;
            break;
        case 2: {
            static const int steps[5] = {-1, 0, 1, 4, 8};   // off, cut, N beats
            int k = 0;
            for (int s = 0; s < 5; s++) if (steps[s] == dd.fade_beats) k = s;
            k = (k + (dir > 0 ? 1 : 4)) % 5;
            dd.fade_beats = steps[k];
            break;
        }
        case 4:
            dd.layout = (dd.layout == DD_LAY_V) ? DD_LAY_H : DD_LAY_V;
            break;
        case DD_ROW_KNOBS:
            dd.knob_mode = (dd.knob_mode == DD_KNOB_CTX) ? DD_KNOB_FIXED : DD_KNOB_CTX;
            break;
        case DD_ROW_FLOCK:
            dd.fader_lock = !dd.fader_lock;
            break;
        case 3: {
            int k = 4;                      // default 1 beat (4 quarters)
            for (int s = 0; s < DD_LOOP_STEPS; s++)
                if (dd_loop_q[s] == dd.loop_len_beats) k = s;
            k = (k + (dir > 0 ? 1 : DD_LOOP_STEPS - 1)) % DD_LOOP_STEPS;
            dd.loop_len_beats = dd_loop_q[k];   // next engage uses it
            break;
        }
    }
}

static int dd_setup_action(int i){
    if (i == DD_ROW_CVMAP) return M_DD_CV;                 // its own page (drums idiom)
    return 0;
}

static setup_menu_t dd_setup = {
    .items       = dd_setup_items,
    .n           = DD_SETUP_N,
    .title       = "DoubleDecker Setup",
    .aff_label   = "Machine",
    .aff_target  = M_MORE,
    .live_target = M_DD_LIVE,
    .render      = setup_value_str,
    .adjust      = setup_adj,
    .action      = dd_setup_action,
};

static int dd_setup_handler(int it_id, int event, void *ev_data){
    (void)it_id; (void)ev_data;
    return setup_menu_event(&dd_setup, event);
}

// ---- registration ---------------------------------------------------------
// ---- CV MAP (Arlo: "selectable cv for the functions assignable in each deck.
// sub menu like the drums"). Every performable function names its own channel.
// The two loops get their OWN pair PER DECK — so put them on free CVs and both
// decks can be worked at once, instead of the trigs' focused-deck compromise.
// Leave them on the filter's and fader's channels (the default) and you get the
// old behaviour: the loop BORROWS those knobs and hands them back by pickup.
static const char *const cv_labels[] = {
    "Crossfade", "Filter", "1 Loop Pos", "1 Loop Len", "2 Loop Pos", "2 Loop Len",
};
#define DD_CV_N 6

static volatile int *cv_slot(int i){
    switch(i){
        case 0: return &dd.cv_fader;
        case 1: return &dd.cv_filt;
        case 2: return &dd.cv_lpos[0];
        case 3: return &dd.cv_llen[0];
        case 4: return &dd.cv_lpos[1];
        case 5: return &dd.cv_llen[1];
    }
    return &dd.cv_filt;
}

#define CV_ROW_Y(i) (TFT_getfontheight() + 14 + (i) * (TFT_getfontheight() + 8))

static void cv_row_redraw(int i, int pos, int sel){
    int fh = TFT_getfontheight();
    int y = CV_ROW_Y(i);
    bool editing = (i == pos && sel);
    _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
    _fg = editing ? TFT_CYAN : TFT_WHITE;
    TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
    TFT_print((char*)cv_labels[i], 8, y);
    int ch = (*cv_slot(i)) & 7;
    char raw[16], val[20];
    snprintf(raw, sizeof(raw), "CV%d", ch + 1);
    if (editing) snprintf(val, sizeof(val), "[ %s ]", raw);
    else snprintf(val, sizeof(val), "%s", raw);
    // AMBER = this knob gets BORROWED (it shares the filter's or the fader's
    // channel while a loop is engaged). RED = the assignment is BROKEN: a control on
    // the CLOCK channel reads the pulse train, not a knob — pulses grab the
    // reference and the gaps between them remap the loop, which collapses it to a
    // stutter within milliseconds. The engine ignores such a control outright; the
    // page has to say so rather than let it look assigned.
    bool on_clock = (dd.clk_src <= 7 && ch == dd.clk_src);
    bool shares = (i >= 2) && (ch == (dd.cv_filt & 7) || ch == (dd.cv_fader & 7));
    _fg = editing ? TFT_CYAN
                  : on_clock ? (color_t){230, 70, 70}
                  : shares   ? (color_t){190, 160, 70}
                             : TFT_WHITE;
    TFT_print(val, _width - TFT_getStringWidth(val) - 10, y);
    _bg = TFT_BLACK;
}

static void cv_redraw(int pos, int sel){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("DoubleDecker CV Map", 6, 4);
    if (dd.knob_mode == DD_KNOB_CTX) {
        // Do not show a map that the engine is ignoring — that is how a UI lies.
        _fg = (color_t){230, 170, 0};
        TFT_setFont(DEF_SMALL_FONT, NULL);
        TFT_print("KNOBS = CONTEXTUAL: this map is not in force", 6,
                  TFT_getfontheight() + 6);
        TFT_setFont(DEFAULT_FONT, NULL);
        _fg = TFT_WHITE;
    }
    for (int i = 0; i < DD_CV_N; i++) cv_row_redraw(i, pos, sel);
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("amber: borrowed while looping   red: on the CLOCK channel (ignored)", 6,
              _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int dd_cv_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; cv_redraw(pos, sel); break;
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if(sel){
                volatile int *slot = cv_slot(pos);
                *slot = (*slot + (dir > 0 ? 1 : 7)) & 7;
                // Re-target a LIVE loop knob and it must go DEAD until moved again,
                // or the newly-assigned knob's current position instantly becomes a
                // window/length command and the loop jumps.
                if (pos >= 2) dualdeck_rearm_loop_knobs((pos - 2) / 2);
                cv_redraw(pos, sel);        // sharing marks can change on any row
            } else {
                pos += dir;
                if(pos >= DD_CV_N) pos = 0;
                if(pos < 0) pos = DD_CV_N - 1;
                cv_redraw(pos, sel);
            }
            break;
        }
        case EV_SHORT_PRESS:
            sel = !sel;
            cv_row_redraw(pos, pos, sel);
            break;
        case EV_LONG_PRESS: return M_DD_SETUP;
        default: break;
    }
    return 0;
}

static void dd_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_DD_LIVE);
    menusys_item_set_default_cb(_ms, M_DD_LIVE, dd_live_handler);
    menusys_new_item(_ms, M_DD_SETUP);
    menusys_item_set_default_cb(_ms, M_DD_SETUP, dd_setup_handler);
    menusys_new_item(_ms, M_DD_LOAD);
    menusys_item_set_default_cb(_ms, M_DD_LOAD, dd_load_handler);
    menusys_new_item(_ms, M_DD_CV);
    menusys_item_set_default_cb(_ms, M_DD_CV, dd_cv_handler);
}

static const char *const dd_main_items[] = {"Live", "Setup"};
static const int dd_main_targets[] = {M_DD_LIVE, M_DD_SETUP};

const machine_ui_t dualdeck_menu_ui = {
    .main_items = dd_main_items,
    .main_targets = dd_main_targets,
    .n_main = 2,
    .register_pages = dd_register_pages,
    .boot_target = M_DD_LIVE,
};
