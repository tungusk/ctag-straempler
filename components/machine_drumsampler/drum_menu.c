// Drum sampler UI — hub-less, like the deck and the sampler: Live pad grid and
// Setup toggle with a long press, the Machine affordance lives in Setup, and
// the per-pad editor (Pads) is a sub-page of Setup with the sample browser
// under it.
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
#include "sample_ram.h"
#include "sample_browser.h"
#include "setup_menu.h"
#include "drum_priv.h"

// the sampler's colour scheme, exactly: BLACK screen, and the pads carry the
// dark panel blue (Arlo — the pads were the grey thing, not the background)
static const color_t SCREEN_BG = {0, 0, 0};
static const color_t PAD_IDLE  = {14, 14, 20};   // = sampler3 PANEL_BG
static const color_t PAD_EMPTY = {8, 8, 12};
static const color_t WF_WHITE  = {235, 235, 235}; // no playhead here to compete with
static const color_t ACCENT_F  = {40, 200, 230};  // the filter's sweep strip
static const color_t WF_B      = {120, 180, 235}; // the B layer: hue IS its identity
// hit flash cycles red -> green -> blue per trigger
static const color_t PAD_LIT[3] = {{230, 60, 50}, {40, 200, 90}, {60, 120, 240}};
static uint8_t s_flash_col[DR_PADS];

// the selected pad lives in dr.sel_pad: the Live encoder picks it, CV6/CV7
// perform it (the engine reads it), and the Pads editor + browser act on it
static int s_load_ret = M_DRUM_PADS;   // page the sample browser returns to
static int s_layer = 0;                // Pads page + browser: which layer they edit


// ---- Live page: pad grid ----------------------------------------------------
static bool s_lit[DR_PADS][DR_LAYERS];

static void pad_cell_rect(int i, int *x, int *y, int *w, int *h){
    int cols = 2, rows = 2;               // 2x2, always
    int fh = TFT_getfontheight();
    int gx = 6, gy = fh + 14;
    int gw = _width - 12, gh = _height - gy - fh - 10;
    *w = gw / cols - 4;
    *h = gh / rows - 4;
    *x = gx + (i % cols) * (gw / cols) + 2;
    *y = gy + (i / cols) * (gh / rows) + 2;
}

// Cell layout. The hit dots CLUSTER at the screen centre (Arlo) — each sits in
// the corner of its cell facing the middle of the grid, which is what makes the
// four of them read as one rhythmic group. The numeral takes the corner
// diagonally opposite, and the trigger source the free corner beside it. Text
// stays SMALL: the big font crowded the cell whichever way it was arranged, and
// the space it was eating is better spent on the waveform.
static void pad_corners(int i, int x, int y, int w, int h,
                        int *dot_cx, int *dot_cy, int *num_left, int *num_top)
{
    int cols = 2;
    int col = i % cols, row = i / cols;
    int r = 9;
    bool dot_right = (col < cols / 2);      // dot hugs the screen centre...
    bool dot_bottom = (row == 0);
    *dot_cx = dot_right ? x + w - r - 5 : x + r + 5;
    *dot_cy = dot_bottom ? y + h - r - 5 : y + r + 5;
    *num_left = dot_right;                  // ...so the numeral takes the far corner
    *num_top  = dot_bottom;
}

// A LAYERED pad is two self-contained half-cells stacked in one window: each half
// carries its own hit dot, sample name, trigger label and waveform (Arlo). A plain
// pad keeps the original anatomy — one dot, one caption, one big waveform.
// A is ALWAYS the upper half. (The bottom row was briefly mirrored — but then the
// encoder's circular path stops being circular: going down the right column you
// meet BR's lower half first, which would be its A, and the ring below reads
// 4B-before-4A. Arlo's ring is the constraint; the layout follows it.)
static bool layer_is_top(int i, int l){
    (void)i;
    return l == 0;
}

static bool half_lit(int i, int l){
    dr_pad_t *p = &dr.pad[i];
    return p->playing && p->cur == (uint8_t)l;
}

// the band a half owns (or the whole cell, for a plain pad)
static void half_rect(int i, int l, int *hx, int *hy, int *hw, int *hh){
    int x, y, w, h;
    pad_cell_rect(i, &x, &y, &w, &h);
    if (!dr.pad[i].layered){ *hx = x; *hy = y; *hw = w; *hh = h; return; }
    int mid = y + h / 2;
    bool top = layer_is_top(i, l);
    *hx = x; *hw = w;
    *hy = top ? y : mid;
    *hh = top ? mid - y : y + h - mid;
}

static void half_dot_pos(int i, int l, int *cx, int *cy, int *r, bool *num_left){
    int hx, hy, hw, hh;
    half_rect(i, l, &hx, &hy, &hw, &hh);
    int cols = 2;
    int col = i % cols, row = i / cols;
    bool dot_right = (col < cols / 2);        // dots still hug the screen centre
    *r = dr.pad[i].layered ? 5 : 9;
    *num_left = dot_right;
    *cx = dot_right ? hx + hw - *r - 5 : hx + *r + 5;
    if (dr.pad[i].layered){
        // the dot rides its half's detail row, which is the cell's OUTER edge
        *cy = layer_is_top(i, l) ? hy + *r + 4 : hy + hh - *r - 4;
    } else {
        *cy = (row == 0) ? hy + hh - *r - 5 : hy + *r + 5;
    }
}

static void pad_dot_layer(int i, int l, bool on){
    dr_pad_t *p = &dr.pad[i];
    int cx, cy, r;
    bool nl;
    half_dot_pos(i, l, &cx, &cy, &r, &nl);
    color_t idle = p->ly[l].len ? PAD_IDLE : PAD_EMPTY;
    TFT_fillCircle(cx, cy, r, on ? PAD_LIT[s_flash_col[i] % 3] : idle);
    s_lit[i][l] = on;
}

// the peak thumbnail (built in RAM at load). WHITE — unlike the sampler's bar there
// is no playhead to compete with (Arlo), and no transport bar either: a one-shot is
// over before a playhead would move.
static void half_waveform(int i, int l, int bx, int by, int bw, int bh){
    dr_layer_t *L = &dr.pad[i].ly[l];
    if (!L->wf_valid || !L->len || bh < 4) return;
    color_t c = (l == 0) ? WF_WHITE : WF_B;   // hue is the layer's identity
    for (int cc = 0; cc < bw - 1; cc += 2){
        float a = sqrtf((float)L->wf[(cc * DR_WF_W) / bw] / 255.0f);
        int ch = (int)(a * (float)bh);
        if (ch < 2) ch = 2;
        TFT_fillRect(bx + cc, by + (bh - ch) / 2, 2, ch, c);
    }
}

// the HALF-WAVE: rectified, growing from a baseline. In a layered cell the two
// layers share the cell's midline as their baseline and grow away from each other
// (A up, B down), which is what gives the split window its definition — the detail
// rows sit outside them, above and below.
static void half_wave_from(int i, int l, int bx, int base, int bw, int reach, bool up){
    dr_layer_t *L = &dr.pad[i].ly[l];
    if (!L->wf_valid || !L->len || reach < 3) return;
    color_t c = (l == 0) ? WF_WHITE : WF_B;
    for (int cc = 0; cc < bw - 1; cc += 2){
        float a = sqrtf((float)L->wf[(cc * DR_WF_W) / bw] / 255.0f);
        int ch = (int)(a * (float)reach);
        if (ch < 2) ch = 2;
        TFT_fillRect(bx + cc, up ? base - ch : base, 2, ch, c);
    }
}

// one half of a layered cell: caption row (name centred, trigger label in the outer
// corner, dot in the inner corner) and the waveform under it
static void draw_half(int i, int l, bool sel_half){
    dr_pad_t *p = &dr.pad[i];
    dr_layer_t *L = &p->ly[l];
    int hx, hy, hw, hh;
    half_rect(i, l, &hx, &hy, &hw, &hh);
    int cx, cy, r;
    bool num_left;
    half_dot_pos(i, l, &cx, &cy, &r, &num_left);

    _bg = L->len ? PAD_IDLE : PAD_EMPTY;
    TFT_fillRect(hx + 1, hy + 1, hw - 2, hh - 2, _bg);

    char nm[10], tag[20];
    snprintf(nm, sizeof(nm), "%.8s", L->sample[0] ? L->sample : "-");
    // pad + layer + its trigger, in one compact tag: "1A CV1"
    if (dr.cv_select)
        snprintf(tag, sizeof(tag), "%d%c %s", i + 1, 'A' + l, l ? "TR2" : "TR1");
    else if (L->trig_src == DR_SRC_NONE)
        snprintf(tag, sizeof(tag), "%d%c --", i + 1, 'A' + l);
    else
        snprintf(tag, sizeof(tag), "%d%c CV%d", i + 1, 'A' + l, (L->trig_src & 7) + 1);

    Font f = cfont;
    TFT_setFont(DEF_SMALL_FONT, NULL);
    int fh = TFT_getfontheight();
    int ty = layer_is_top(i, l) ? hy + 3 : hy + hh - fh - 3;   // details on the outer edge
    _fg = sel_half ? TFT_WHITE : ((l == 0) ? (color_t){110, 130, 170} : WF_B);
    TFT_print(tag, num_left ? hx + 5 : hx + hw - TFT_getStringWidth(tag) - 5, ty);
    _fg = L->len ? TFT_WHITE : (color_t){110, 110, 110};
    // the name is centred in what the tag and the dot leave — not in the half, or a
    // long name would run under both
    TFT_print(nm, hx + hw / 2 - TFT_getStringWidth(nm) / 2, ty);
    cfont = f;
    TFT_setFont(DEFAULT_FONT, NULL);

    pad_dot_layer(i, l, half_lit(i, l));
    (void)fh;
}

static void draw_pad_cell(int i, bool lit){
    int x, y, w, h, cx, cy, num_left, num_top;
    pad_cell_rect(i, &x, &y, &w, &h);
    pad_corners(i, x, y, w, h, &cx, &cy, &num_left, &num_top);
    dr_pad_t *p = &dr.pad[i];
    // sel_pad KEEPS pointing at its pad while the encoder is on the filter box (so
    // the knobs know where to return) — so "selected" is sel_pad AND not-the-box,
    // or the pad stays outlined and two things look selected at once
    bool sel = (i == dr.sel_pad) && !dr.sel_filter;
    color_t border = p->enabled ? (color_t){90, 90, 110} : (color_t){50, 50, 60};

    if (p->layered){
        int cmid = y + h / 2;
        TFT_fillRect(x, y, w, h, PAD_EMPTY);
        draw_half(i, 0, sel && s_layer == 0);
        draw_half(i, 1, sel && s_layer == 1);
        // the two half-waves CONVERGE on the midline: A grows up to meet it, B grows
        // down from it, with each half's details outside them
        {
            int sfh = 0;
            Font ff = cfont;
            TFT_setFont(DEF_SMALL_FONT, NULL);
            sfh = TFT_getfontheight();
            cfont = ff;
            TFT_setFont(DEFAULT_FONT, NULL);
            int rr = 5;
            int top_floor = y + 3 + sfh + 2;            // under the upper detail row
            int t_dotb    = y + 3 + rr * 2 + 2;
            if (t_dotb > top_floor) top_floor = t_dotb;
            int bot_ceil  = y + h - 3 - sfh - 2;        // over the lower detail row
            int b_dott    = y + h - 3 - rr * 2 - 2;
            if (b_dott < bot_ceil) bot_ceil = b_dott;
            int lt = layer_is_top(i, 0) ? 0 : 1;        // the layer in the upper half
            int lb = 1 - lt;
            half_wave_from(i, lt, x + 5, cmid - 2, w - 10, cmid - 2 - top_floor, true);
            half_wave_from(i, lb, x + 5, cmid + 2, w - 10, bot_ceil - (cmid + 2), false);
        }
        // the cell's own border stays GREY even when a half is selected — only the
        // selected HALF goes white, or the unselected half looks selected too (Arlo)
        _fg = border;
        TFT_drawRect(x, y, w, h, _fg);
        // the divide: BLACK and wide enough to be a gap, so the two halves read as
        // two windows rather than one
        TFT_fillRect(x + 1, cmid - 1, w - 2, 3, SCREEN_BG);
        if (sel){
            int sy0 = (s_layer == 1) ? cmid : y;
            int sh  = h / 2;
            _fg = TFT_WHITE;
            TFT_drawRect(x + 1, sy0 + 1, w - 2, sh - 2, _fg);
            TFT_drawRect(x + 2, sy0 + 2, w - 4, sh - 4, _fg);
        }
        return;
    }

    // ---- a plain pad: the original anatomy, untouched ----
    _bg = p->ly[0].len ? PAD_IDLE : PAD_EMPTY;  // static cell; the dot does the flashing
    TFT_fillRect(x, y, w, h, _bg);
    _fg = sel ? TFT_WHITE : border;
    TFT_drawRect(x, y, w, h, _fg);
    if (sel){
        TFT_drawRect(x + 1, y + 1, w - 2, h - 2, _fg);
        TFT_drawRect(x + 2, y + 2, w - 4, h - 4, _fg);
    }

    char nm[10], n[16], src[16];
    snprintf(nm, sizeof(nm), "%.8s", p->ly[0].sample[0] ? p->ly[0].sample : "-");
    snprintf(n, sizeof(n), "%d", i + 1);
    int sa = p->ly[0].trig_src;
    if (dr.cv_select) snprintf(src, sizeof(src), "sel");
    else if (sa == DR_SRC_NONE) snprintf(src, sizeof(src), "--");
    else snprintf(src, sizeof(src), "CV%d", (sa & 7) + 1);            // 1..8, always
    Font f = cfont;

    // numeral + name on the edge opposite the dot: numeral in the outer corner,
    // name centred beside it
    TFT_setFont(DEFAULT_FONT, NULL);
    int fh = TFT_getfontheight();
    int ty = num_top ? y + 4 : y + h - fh - 4;
    _fg = p->enabled ? TFT_WHITE : (color_t){90, 90, 90};
    TFT_print(n, num_left ? x + 6 : x + w - TFT_getStringWidth(n) - 6, ty);
    _fg = p->ly[0].len ? TFT_WHITE : (color_t){110, 110, 110};
    TFT_print(nm, x + w / 2 - TFT_getStringWidth(nm) / 2, ty);

    // trigger source, small, in the free corner: the dot's edge, outer side
    TFT_setFont(DEF_SMALL_FONT, NULL);
    int sfh = TFT_getfontheight();
    int sy = num_top ? y + h - sfh - 4 : y + 4;
    _fg = (color_t){110, 130, 170};
    TFT_print(src, num_left ? x + 6 : x + w - TFT_getStringWidth(src) - 6, sy);
    cfont = f;
    TFT_setFont(DEFAULT_FONT, NULL);

    // the waveform gets EVERYTHING left over — every pixel between the two text
    // rows, full cell width. It is the point of the cell; the text is a caption.
    {
        int r = 9;
        int dot_top = cy - r, dot_bot = cy + r;
        int top, bot;
        if (num_top){                                   // caption on top, dot below
            top = ty + fh + 3;
            bot = (sy < dot_top ? sy : dot_top) - 3;
        } else {                                        // dot on top, caption below
            int under = sy + sfh > dot_bot ? sy + sfh : dot_bot;
            top = under + 3;
            bot = ty - 3;
        }
        half_waveform(i, 0, x + 5, top, w - 10, bot - top);
    }
    pad_dot_layer(i, 0, lit);
}

// ---- the master filter box (menu bar, top right) -----------------------------
// It is the fifth thing the encoder can land on. Same selection language as a
// pad (bold triple-stroke outline), same loaded/empty language for on/off (the
// pad blue when live, black when bypassed).
#define FBOX_W 84

static int s_fbox_mode = -1, s_fbox_cell = -1, s_fbox_on = -1, s_fbox_sel = -1;

static void filter_label(char *s, size_t n){
    // in CV-select mode the selectors default to EXACTLY knob6/knob7, so the
    // filter knobs are blocked. Say so — a dead knob reads as broken hardware.
    bool blocked = dr.cv_select &&
                   (dr.sel_src[0] == DR_MOD_LEVEL_CV || dr.sel_src[1] == DR_MOD_LEVEL_CV);
    // the box always SAYS what it is (Arlo). The mode rides along once it is
    // sweeping, and the strip underneath shows how far the sweep has gone — the
    // cutoff in Hz was detail nobody was reading mid-performance.
    if (blocked)          { snprintf(s, n, "Filter sel"); return; }
    if (!dr.flt_on)       { snprintf(s, n, "Filter");     return; }
    if (dr.flt_mode == 0) { snprintf(s, n, "Filter");     return; }
    snprintf(s, n, "Filter %s", (dr.flt_mode == 1) ? "LP" : "HP");
}

static void draw_filter_box(void){
    if (!dr.flt_box) return;
    bool sel = dr.sel_filter;
    int fh = TFT_getfontheight();
    int bh = fh + 8, bw = FBOX_W;
    int bx = _width / 2 - bw / 2, by = 2;   // dead centre of the menu bar: it sits
                                            // between pads 1 and 2 in the ring too
    _bg = dr.flt_on ? PAD_IDLE : SCREEN_BG;   // filled = engaged (a pad's grammar)
    TFT_fillRect(bx, by, bw, bh, _bg);
    _fg = sel ? TFT_WHITE : (dr.flt_on ? (color_t){90, 90, 110} : (color_t){60, 60, 70});
    TFT_drawRect(bx, by, bw, bh, _fg);
    if (sel){                                  // the pad's bold selection outline
        TFT_drawRect(bx + 1, by + 1, bw - 2, bh - 2, _fg);
        TFT_drawRect(bx + 2, by + 2, bw - 4, bh - 4, _fg);
    }
    char lab[16];
    filter_label(lab, sizeof(lab));
    TFT_setFont(DEF_SMALL_FONT, NULL);
    _fg = dr.flt_on ? TFT_WHITE : (color_t){120, 120, 140};
    TFT_print(lab, bx + bw / 2 - TFT_getStringWidth(lab) / 2, by + 4);
    TFT_setFont(DEFAULT_FONT, NULL);
    // sweep strip: a centre tick with the depth filled out toward LP or HP
    int sy = by + bh - 3, sx = bx + 4, sw = bw - 8;
    TFT_fillRect(sx, sy, sw, 2, SCREEN_BG);
    TFT_fillRect(sx + sw / 2, sy, 1, 2, (color_t){80, 80, 100});
    if (dr.flt_on && dr.flt_mode){
        int d = (dr.flt_cv - 2048) * (sw / 2) / 2048;
        if (d < 0) TFT_fillRect(sx + sw / 2 + d, sy, -d, 2, ACCENT_F);
        else       TFT_fillRect(sx + sw / 2, sy, d, 2, ACCENT_F);
    }
    _bg = SCREEN_BG;
    s_fbox_mode = dr.flt_mode;
    s_fbox_cell = dr.flt_cv >> 5;       // repaint only on a VISIBLE move
    s_fbox_on   = dr.flt_on ? 1 : 0;
    s_fbox_sel  = sel ? 1 : 0;
}

// repaint the box only when something on it actually changed — it sits on the
// fast timer, and an unconditional redraw strobes
static void filter_box_tick(void){
    if (!dr.flt_box) return;
    if (dr.flt_mode != s_fbox_mode || (dr.flt_cv >> 5) != s_fbox_cell ||
        (dr.flt_on ? 1 : 0) != s_fbox_on || (dr.sel_filter ? 1 : 0) != s_fbox_sel)
        draw_filter_box();
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(SCREEN_BG);
    _bg = SCREEN_BG; _fg = TFT_WHITE;
    TFT_print("Drums", 6, 4);
    for (int i = 0; i < DR_PADS; i++) draw_pad_cell(i, dr.pad[i].playing);
    s_fbox_mode = -1;                      // force the box to paint
    draw_filter_box();
    _bg = SCREEN_BG;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    const char *hint = dr.sel_filter
        ? "turn:select  press:filter on/off  knob6:sweep  knob7:res"
        : "turn:select  press:load  hold:setup  knob6/7: level/decay";
    TFT_print((char*)hint, 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

// turn = pick the pad (or the filter box) the outline and CV6/CV7 follow; only
// what changed is repainted — a full grid repaint per detent strobes
// The encoder walks every LOADABLE slot: a plain pad is one, a LAYERED pad is two
// (its A half and its B half), then the filter box. Layering is per pad, so the
// slot count is a sum, not a multiplication.
//
// The walk goes ROUND the grid, not along the array (Arlo). Pad index order is
// 1,2,3,4 = top-left, top-right, bottom-left, bottom-right — turning the encoder
// through that jumps the cursor diagonally across the screen on every second
// detent. Visiting them clockwise instead (TL, TR, BR, BL) makes the knob feel
// like it is tracing the grid, and within a pad the halves run top-then-bottom,
// which is the same direction the eye is already travelling.
// The circle Arlo wants, traced on the grid (pads 1..4 = TL, TR, BL, BR):
//
//   1A  ->  FILT  ->  2A          across the top, through the box in the middle
//                     2B          down the right column...
//                     4A  4B      ...through pad 4
//   1B  <-  3A  <-    3B          back along the bottom and up the left column
//
// So a half is only ever adjacent to the half it physically touches. The table is
// (pad, layer) pairs; -1 is the filter box; B entries are skipped for a pad that
// isn't layered.
static const int8_t RING[][2] = {
    {0, 0}, {-1, 0}, {1, 0}, {1, 1}, {3, 0}, {3, 1}, {2, 1}, {2, 0}, {0, 1},
};
#define RING_N ((int)(sizeof(RING) / sizeof(RING[0])))

// The ring is an explicit table, because the filter box lives INSIDE it: the box is
// drawn in the middle of the menu bar, between pads 1 and 2, and the encoder passes
// through it there rather than tacking it on after the last pad (Arlo).
// slot = {pad, layer}; pad == -1 is the filter box.
static int8_t s_slot[DR_PADS * DR_LAYERS + 1][2];
static int s_nslot;

static void build_ring(void){
    s_nslot = 0;
    for (int k = 0; k < RING_N; k++){
        int pad = RING[k][0], ly = RING[k][1];
        if (pad < 0){                                  // the filter box
            if (!dr.flt_box) continue;
        } else if (ly == 1 && !dr.pad[pad].layered) {
            continue;                                  // no B half on a plain pad
        }
        s_slot[s_nslot][0] = (int8_t)pad;
        s_slot[s_nslot][1] = (int8_t)ly;
        s_nslot++;
    }
}

static int live_slot_of(int pad, int layer){
    for (int k = 0; k < s_nslot; k++)
        if (s_slot[k][0] == pad && s_slot[k][1] == layer) return k;
    return 0;
}

static int live_filter_slot(void){
    for (int k = 0; k < s_nslot; k++) if (s_slot[k][0] < 0) return k;
    return -1;
}

static void live_select(int dir){
    build_ring();                              // layering can change between turns
    if (s_nslot == 0) return;
    if (dr.sel_pad >= DR_PADS) dr.sel_pad = 0;
    int prev = dr.sel_filter ? live_filter_slot() : live_slot_of(dr.sel_pad, s_layer);
    if (prev < 0) prev = 0;
    int cur = (prev + dir + s_nslot) % s_nslot;
    if (cur == prev) return;
    int prev_pad = dr.sel_pad;
    bool was_box = dr.sel_filter;
    if (s_slot[cur][0] < 0){                   // landed on the filter box
        dr.sel_filter = true;
        dr.flt_take_f = dr.flt_take_q = false; // arm the knob pickup: the sweep
        dr.flt_ref_f = dr.flt_ref_q = -1;      // must not jump to where a knob sits
    } else {
        dr.sel_filter = false;
        dr.sel_pad = s_slot[cur][0];
        s_layer    = s_slot[cur][1];
    }
    if (!was_box)       draw_pad_cell(prev_pad, dr.pad[prev_pad].playing);
    if (!dr.sel_filter) draw_pad_cell(dr.sel_pad, dr.pad[dr.sel_pad].playing);
    draw_filter_box();
}

static int drum_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            if (dr.sel_pad >= DR_PADS) dr.sel_pad = 0;
            if (!dr.flt_box) dr.sel_filter = false;
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW:
            for (int i = 0; i < DR_PADS; i++){
                dr_pad_t *p = &dr.pad[i];
                int nly = p->layered ? DR_LAYERS : 1;
                if (p->hit){
                    int hl = p->layered ? (p->hit_layer & 1) : 0;
                    p->hit = false;
                    s_flash_col[i]++;               // next hit, next colour
                    pad_dot_layer(i, hl, true);     // dot only — fast enough to groove
                    // a choke means the OTHER half just went dark
                    if (p->layered) pad_dot_layer(i, hl ^ 1, false);
                }
                for (int l = 0; l < nly; l++){
                    bool lit_l = half_lit(i, l);
                    if (lit_l != s_lit[i][l]) pad_dot_layer(i, l, lit_l);
                }
            }
            filter_box_tick();
            break;
        case EV_FWD: live_select(+1); break;
        case EV_BWD: live_select(-1); break;
        case EV_SHORT_PRESS:
            if (dr.sel_filter){                     // the box: click = engage/bypass
                dr.flt_on = !dr.flt_on;
                draw_filter_box();
                break;
            }
            if (!dr.pad[dr.sel_pad].layered) s_layer = 0;   // load into the selected
            s_load_ret = M_DRUM_LIVE;                       // half (A, or B)
            return M_DRUM_LOAD;
        case EV_LONG_PRESS: return M_DRUM_SETUP;   // Live <-> Setup, no hub
        default: break;
    }
    return 0;
}

// ---- shared row style (the sampler's convention) -----------------------------
// A row with a SMALL option set flips in place on a click — no edit mode to
// enter (pad select, ON/OFF, 4-vs-8). Rows whose value is a range keep the
// click-to-edit mode, and while editing the value wears a [ ] bracket, because
// the cyan alone didn't read as a mode.
#define DR_ROW_Y(i) (TFT_getfontheight() + 16 + (i) * (TFT_getfontheight() + 8))

static void row_draw(int i, int pos, int sel, const char *label, const char *raw){
    int fh = TFT_getfontheight();
    int y = DR_ROW_Y(i);
    bool editing = (i == pos && sel);
    _bg = (i == pos) ? (color_t){10, 18, 56} : SCREEN_BG;
    _fg = editing ? TFT_CYAN : TFT_WHITE;
    TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
    TFT_print((char*)label, 8, y);
    char val[32];
    if (editing) snprintf(val, sizeof(val), "[ %s ]", raw);
    else snprintf(val, sizeof(val), "%s", raw);
    TFT_print(val, _width - TFT_getStringWidth(val) - 10, y);
    _bg = SCREEN_BG;
}

// ---- Pads page: per-pad editor (sub-page of Setup) ---------------------------
// Rows are ID-keyed and the visible list is rebuilt per entry (Layer only exists
// when B layers are on). The old index-keyed toggle macro would have silently
// mis-assigned click behaviour the moment a row was inserted.
enum { PR_PAD = 0, PR_LAYERED, PR_LAYER, PR_SAMPLE, PR_TRIG, PR_LEVEL, PR_PAN,
       PR_SEND, PR_DECAY, PR_CW, PR_RETRIG, PR_ENABLED, PR_COUNT };

static const char *pads_labels[PR_COUNT] = {"Pad", "B Layer", "Layer", "Sample",
                                            "Trig In", "Level", "Pan", "Rev Send",
                                            "Decay", "Knob7 CW", "Retrig",
                                            "Enabled"};
// small option sets flip on a click; ranges keep click-to-edit
#define PADS_IS_TOGGLE(id) ((id) == PR_PAD || (id) == PR_LAYERED || \
                            (id) == PR_LAYER || (id) == PR_CW || (id) == PR_ENABLED)

static int s_prows[PR_COUNT], s_pnrows;

static void pads_build_rows(void){
    bool layered = dr.pad[dr.sel_pad].layered;   // layering is per PAD now
    s_pnrows = 0;
    for (int id = 0; id < PR_COUNT; id++){
        if (id == PR_LAYER && !layered) continue;   // nothing to switch between
        s_prows[s_pnrows++] = id;
    }
    if (!layered) s_layer = 0;
}

static void pads_value_str(int id, char *v, size_t n){
    dr_pad_t *p = &dr.pad[dr.sel_pad];
    dr_layer_t *L = &p->ly[s_layer];       // Sample + Trig In address the LAYER;
    switch(id){                            // everything else is the pad
        case PR_PAD:   snprintf(v, n, "%d / %d", dr.sel_pad + 1, DR_PADS); break;
        case PR_LAYERED: snprintf(v, n, "%s", p->layered ? "A+B choke" : "OFF"); break;
        case PR_LAYER: snprintf(v, n, "%s", s_layer ? "B" : "A"); break;
        case PR_SAMPLE: snprintf(v, n, "%s", L->sample[0] ? L->sample : "(none)"); break;
        case PR_TRIG:
            if (L->trig_src == DR_SRC_NONE) snprintf(v, n, "none");
            else snprintf(v, n, "CV%d", (L->trig_src & 7) + 1);
            break;
        // level reads as % of unity: 100 % is the sample as recorded, past that
        // the pad is driven ("drv" = into the soft clipper)
        case PR_LEVEL: {
            int pct = (int)p->level * 100 / DR_LEVEL_UNITY;
            if (p->level > DR_LEVEL_UNITY) snprintf(v, n, "%d%% drv", pct);
            else snprintf(v, n, "%d%%", pct);
            break;
        }
        case PR_SEND:
            if (dr.rv.mode == RV_OFF) snprintf(v, n, "%d%% (rev off)",
                                               (int)p->rv_send * 100 / 255);
            else snprintf(v, n, "%d%%", (int)p->rv_send * 100 / 255);
            break;
        case PR_PAN:
            if (p->pan == 128) snprintf(v, n, "C");
            else if (p->pan < 128) snprintf(v, n, "L%d", (128 - p->pan) * 100 / 128);
            else snprintf(v, n, "R%d", (p->pan - 128) * 100 / 127);
            break;
        // knob7 CCW writes decay; CW writes whichever target PR_CW selects, so each
        // row shows the value that is actually live
        case PR_DECAY:
            if (p->decay_ms == 0) snprintf(v, n, "FULL");
            else snprintf(v, n, "%dms", p->decay_ms);
            break;
        case PR_CW:
            if (p->cw_mode == DR_CW_NONE)
                snprintf(v, n, "none");
            else if (p->cw_mode == DR_CW_START)
                snprintf(v, n, "start %d%%", (int)p->start_off * 100 / 255);
            else if (p->cw_mode == DR_CW_ATTACK)
                snprintf(v, n, "attack %dms", p->attack_ms);
            else if (p->loop_ms)                       // named to match the Retrig row
                snprintf(v, n, "retrig %dms", p->loop_ms);
            else
                snprintf(v, n, "retrig");
            break;
        case PR_RETRIG:
            if (p->loop_reps == DR_REPS_INF) snprintf(v, n, "INF");
            else if (p->loop_reps) snprintf(v, n, "%d", p->loop_reps);
            else snprintf(v, n, "sample");     // run the loop out over the sample
            break;
        case PR_ENABLED: snprintf(v, n, "%s", p->enabled ? "ON" : "OFF"); break;
        default: v[0] = 0;
    }
}

static void pads_row_redraw(int idx, int pos, int sel){
    char raw[24];
    int id = s_prows[idx];
    pads_value_str(id, raw, sizeof(raw));
    row_draw(idx, pos, sel, pads_labels[id], raw);
}

static void pads_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = SCREEN_BG; TFT_fillScreen(SCREEN_BG);
    _fg = TFT_WHITE;
    char h[24];
    snprintf(h, sizeof(h), "Drum Pads");
    TFT_print(h, 6, 4);
    for (int i = 0; i < s_pnrows; i++) pads_row_redraw(i, pos, sel);
    _bg = SCREEN_BG;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print(dr.pad[dr.sel_pad].layered ? "B chokes A: one voice, two sounds"
                                        : "press Sample to open the browser; hold to go back",
              8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

// selector/trigger cycle: none -> CV1..CV8 -> none
static int src_cycle(int src, int dir){
    src += dir;
    if (src > 7) src = DR_SRC_NONE;
    if (src < DR_SRC_NONE) src = 7;
    return src;
}

static void pads_adj(int id, int dir){
    dr_pad_t *p = &dr.pad[dr.sel_pad];
    dr_layer_t *L = &p->ly[s_layer];
    switch(id){
        case PR_PAD:
            dr.sel_pad = (dr.sel_pad + (dir > 0 ? 1 : DR_PADS - 1)) % DR_PADS;
            break;
        case PR_LAYERED:
            p->layered = !p->layered;
            if (!p->layered) s_layer = 0;   // the B half no longer exists to edit
            break;
        case PR_LAYER: s_layer = s_layer ? 0 : 1; break;
        case PR_TRIG:  L->trig_src = src_cycle(L->trig_src, dir); break;
        case PR_LEVEL: {
            int lv = (int)p->level + dir * 16;
            if (lv < 0) lv = 0;
            if (lv > DR_LEVEL_MAX) lv = DR_LEVEL_MAX;   // past unity = drive
            p->level = (uint16_t)lv;
            break;
        }
        case PR_SEND: {
            int sd = (int)p->rv_send + dir * 16;   // ~6% steps
            if (sd < 0) sd = 0;
            if (sd > 255) sd = 255;
            p->rv_send = (uint8_t)sd;
            break;
        }
        case PR_PAN: {
            int pn = (int)p->pan + dir * 16;
            if (pn < 0) pn = 0;
            if (pn > 255) pn = 255;
            p->pan = (uint8_t)pn;
            break;
        }
        case PR_DECAY: {
            int dm = (int)p->decay_ms + dir * 50;
            if (dm < 0) dm = 0;
            if (dm > 5000) dm = 5000;
            p->decay_ms = (uint16_t)dm;
            break;
        }
        // switching the CW target retires the others — leaving a stale attack (or
        // skipped head, or retrig) applied to a pad whose knob no longer drives it
        // would be a setting nothing on screen explains
        case PR_CW:
            p->cw_mode = (uint8_t)((p->cw_mode + 1) % DR_CW_MODES);
            p->attack_ms = 0;
            p->start_off = 0;
            p->loop_ms = 0;
            break;
        // the retrig ladder: run the loop out over the sample, a fixed count, or
        // INF — hold the stutter until the pad is hit again
        case PR_RETRIG: {
            static const uint8_t reps[] = {0, 2, 3, 4, 6, 8, 12, 16, DR_REPS_INF};
            const int nr = sizeof(reps) / sizeof(reps[0]);
            int k = 0;
            for (int q = 0; q < nr; q++) if (reps[q] == p->loop_reps) { k = q; break; }
            k += dir;
            if (k < 0) k = nr - 1;
            if (k >= nr) k = 0;
            p->loop_reps = reps[k];
            break;
        }
        case PR_ENABLED: p->enabled = !p->enabled; break;
    }
}

static int drum_pads_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU:
            sel = 0;
            if (dr.sel_pad >= DR_PADS) dr.sel_pad = 0;
            pads_build_rows();
            if (pos >= s_pnrows) pos = 0;
            pads_redraw(pos, sel);
            break;
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if(sel){
                pads_adj(s_prows[pos], dir);
                pads_row_redraw(pos, pos, sel);      // value edit: one row only
            } else {
                pos = (pos + dir + s_pnrows) % s_pnrows;
                pads_redraw(pos, sel);
            }
            break;
        }
        case EV_SHORT_PRESS: {
            int id = s_prows[pos];
            if(id == PR_SAMPLE){ s_load_ret = M_DRUM_PADS; return M_DRUM_LOAD; }
            if(PADS_IS_TOGGLE(id)){
                pads_adj(id, +1);                    // small option set: flip in place
                // pad / layer / layering all change what the whole page shows —
                // and B Layer adds or removes a row, so the list must be rebuilt
                if (id == PR_PAD || id == PR_LAYER || id == PR_LAYERED){
                    pads_build_rows();
                    if (pos >= s_pnrows) pos = s_pnrows - 1;
                    pads_redraw(pos, sel);
                } else pads_row_redraw(pos, pos, 0);
            } else {
                sel = !sel; pads_row_redraw(pos, pos, sel);
            }
            break;
        }
        case EV_LONG_PRESS: return M_DRUM_SETUP;   // Pads is a sub-page of Setup
        default: break;
    }
    return 0;
}

// ---- Load browser: the shared two-level widget (folders -> big-name list) --
static int drum_load_handler(int it_id, int event, void *ev_data){
    if (event == EV_ENTERED_MENU){
        char t[32];
        if (dr.pad[dr.sel_pad].layered)
            snprintf(t, sizeof(t), "Pad %d%c", dr.sel_pad + 1, s_layer ? 'B' : 'A');
        else
            snprintf(t, sizeof(t), "Pad %d", dr.sel_pad + 1);
        // Drums opens into its home folder (usr/DRUMS) — kits live there
        sample_browser_enter_dir(true, t, dr.pad[dr.sel_pad].ly[s_layer].sample, SAMPLE_DIR_DRUMS);
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1){
        // re-loading the sample the layer already holds costs an SD read and
        // rebuilds the thumbnail for nothing (Arlo) — confirming is a no-op
        const char *sel = sample_browser_selected();
        if (strcmp(sel, dr.pad[dr.sel_pad].ly[s_layer].sample) != 0)
            drum_load_layer(dr.sel_pad, s_layer, (char*)sel);
        return s_load_ret;
    }
    if (r == 2) return s_load_ret;   // cancel without loading
    return 0;
}

// ---- Setup page (shared framework: TOGGLE press-cycles, RANGE [ ] edits, ------
// ACTION opens a sub-page) ------------------------------------------------------
// Row PADEDIT opens the per-pad editor (Arlo: Pads belongs under Setup, not in
// a hub). The two selector-CV rows exist ONLY in CV-select mode — in Direct
// mode the selectors do nothing, so the page doesn't mention them at all and
// the encoder walks straight past. The visible list (and thus the framework's
// items/n) is rebuilt whenever the trigger mode changes; s_rows[] maps a visible
// index back to its R_* id for the render/adjust/action callbacks.
enum { R_PADEDIT = 0, R_TRIG, R_SENS, R_SEL1, R_SEL2, R_VEL,
       R_KNOB, R_FILTER, R_REVERB, R_RVMIX, R_RVTAP, R_COUNT };

// the full kind table: Pad Setup opens a page (ACTION), the two selector CVs are
// wide (none + 8 channels) so they keep click-to-edit (RANGE) as does Rev Return
// (a %); everything else is a small option set (TOGGLE)
static const setup_item_t dr_all_items[R_COUNT] = {
    [R_PADEDIT] = {"Pad Setup",  ST_ACTION},
    [R_TRIG]    = {"Trigger",    ST_TOGGLE},
    [R_SENS]    = {"Sensi",      ST_TOGGLE},
    [R_SEL1]    = {"Sel CV TR1", ST_RANGE},
    [R_SEL2]    = {"Sel CV TR2", ST_RANGE},
    [R_VEL]     = {"Velocity",   ST_TOGGLE},
    [R_KNOB]    = {"Knob 6/7",   ST_TOGGLE},
    [R_FILTER]  = {"Filter",     ST_TOGGLE},
    [R_REVERB]  = {"Reverb",     ST_TOGGLE},
    [R_RVMIX]   = {"Rev Return", ST_RANGE},
    [R_RVTAP]   = {"Send Tap",   ST_TOGGLE},
};

// visible rows, rebuilt whenever the trigger mode changes; s_rows[k] = the R_* id
// shown at visible index k, and dr_vis_items[] is the framework's items array
static int s_rows[R_COUNT], s_nrows;
static setup_item_t dr_vis_items[R_COUNT];
static setup_menu_t dr_setup;

static void setup_build_rows(void){
    s_nrows = 0;
    for (int id = 0; id < R_COUNT; id++){
        if ((id == R_SEL1 || id == R_SEL2) && !dr.cv_select) continue;
        s_rows[s_nrows] = id;
        dr_vis_items[s_nrows] = dr_all_items[id];
        s_nrows++;
    }
    dr_setup.items = dr_vis_items;
    dr_setup.n = s_nrows;
}

static void sel_src_str(int src, char *v, size_t n){
    if (src < 0) snprintf(v, n, "none");
    else snprintf(v, n, "CV%d", (src & 7) + 1);
}

static void setup_value_str(int id, char *v, size_t n){
    switch(id){
        case R_PADEDIT: v[0] = 0; break;      // a page link: no value to show
        case R_TRIG: snprintf(v, n, "%s", dr.cv_select ? "CV-select" : "Direct"); break;
        case R_SENS: snprintf(v, n, "%s", dr.sens == 0 ? "Low" : (dr.sens == 2 ? "High" : "Med")); break;
        case R_SEL1: sel_src_str(dr.sel_src[0], v, n); break;
        case R_SEL2: sel_src_str(dr.sel_src[1], v, n); break;
        case R_VEL:  snprintf(v, n, "%s", dr.velocity ? "ON" : "OFF"); break;
        case R_KNOB: snprintf(v, n, "%s", dr.cv_mod ? "level/decay" : "OFF"); break;
        case R_FILTER: snprintf(v, n, "%s", dr.flt_box ? "box" : "OFF"); break;
        case R_REVERB:
            if (dr.rv.mode == RV_OFF) snprintf(v, n, "OFF");
            else snprintf(v, n, "%s %dus", reverb_mode_name(dr.rv.mode), dr.rv.cost_us);
            break;
        case R_RVMIX: snprintf(v, n, "%d%%", (int)(dr.rv.wet * 100 + 0.5f)); break;   // RETURN
        case R_RVTAP: snprintf(v, n, "%s", dr.rv_post ? "post-filter" : "pre-filter"); break;
        default: v[0] = 0;
    }
}

// selector CV cycles through none -> CV1..CV8 -> none: a trig with no selector
// simply doesn't fire (the sampler's src_cycle, same shape)
static int sel_cycle(int src, int dir){
    src += dir;
    if (src > 7) src = -1;
    if (src < -1) src = 7;
    return src;
}

static void setup_adj(int id, int dir){
    switch(id){
        case R_TRIG: dr.cv_select = !dr.cv_select; break;
        case R_SENS: dr.sens = (dr.sens + (dir > 0 ? 1 : 2)) % 3; break;
        case R_SEL1: dr.sel_src[0] = sel_cycle(dr.sel_src[0], dir); break;
        case R_SEL2: dr.sel_src[1] = sel_cycle(dr.sel_src[1], dir); break;
        case R_VEL:  dr.velocity = !dr.velocity; break;
        case R_KNOB: dr.cv_mod = !dr.cv_mod; break;
        case R_FILTER:
            dr.flt_box = !dr.flt_box;
            if (!dr.flt_box) dr.sel_filter = false;   // don't strand the encoder on a
            break;                                    // box that no longer exists
        case R_REVERB: {
            int m = (dr.rv.mode + (dir > 0 ? 1 : RV_N_MODES - 1)) % RV_N_MODES;
            // lazy slab on first use; a failed PSRAM alloc fails soft to OFF
            if (m != RV_OFF && !dr.rv.slab && reverb_init(&dr.rv) != ESP_OK) m = RV_OFF;
            reverb_set_mode(&dr.rv, m);
            break;
        }
        case R_RVMIX: reverb_set_mix(&dr.rv, dr.rv.wet + (float)dir * 0.05f); break;
        case R_RVTAP: dr.rv_post = !dr.rv_post; break;   // flip it while playing
    }
}

// framework callbacks — all index the VISIBLE row; s_rows maps it to the R_* id
static void dr_setup_render(int i, char *v, size_t n){ setup_value_str(s_rows[i], v, n); }

static void dr_setup_adj(int i, int dir){
    int id = s_rows[i];
    setup_adj(id, dir);
    // Trigger mode adds/removes the two selector rows: rebuild the visible list
    // so the framework's next draw reflects it (Trigger sits above them, so pos
    // stays valid).
    if (id == R_TRIG) setup_build_rows();
}

static int dr_setup_action(int i){
    if (s_rows[i] == R_PADEDIT) return M_DRUM_PADS;   // per-pad editor
    return 0;
}

static setup_menu_t dr_setup = {
    .title = "Drum Setup",
    .aff_label = "Machine", .aff_target = M_MORE,
    .live_target = M_DRUM_LIVE,
    .render = dr_setup_render, .adjust = dr_setup_adj, .action = dr_setup_action,
    // .items / .n are set by setup_build_rows() on entry (mode-dependent)
};

static int drum_setup_handler(int it_id, int event, void *ev_data){
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) setup_build_rows();   // list depends on cv_select
    return setup_menu_event(&dr_setup, event);
}

// ---- registration ---------------------------------------------------------
static void drum_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_DRUM_LIVE);  menusys_item_set_default_cb(_ms, M_DRUM_LIVE, drum_live_handler);
    menusys_new_item(_ms, M_DRUM_PADS);  menusys_item_set_default_cb(_ms, M_DRUM_PADS, drum_pads_handler);
    menusys_new_item(_ms, M_DRUM_LOAD);  menusys_item_set_default_cb(_ms, M_DRUM_LOAD, drum_load_handler);
    menusys_new_item(_ms, M_DRUM_SETUP); menusys_item_set_default_cb(_ms, M_DRUM_SETUP, drum_setup_handler);
}

static int drum_main_event(int event, void *ev_data){
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW){
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = (color_t){230, 140, 40};
        int loaded = 0;                            // count LAYERS, not pads
        for (int i = 0; i < DR_PADS; i++)
            for (int l = 0; l < (dr.pad[i].layered ? DR_LAYERS : 1); l++)
                if (dr.pad[i].ly[l].len) loaded++;
        char s[64];
        snprintf(s, sizeof(s), "Drums: %d pads (%d loaded) %s",
                 DR_PADS, loaded, dr.cv_select ? "cv-sel" : "direct");
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

// hub-less: Pads hangs off Setup now, so the (unreachable) main screen only
// lists the two real pages
static const char *const drum_main_items[] = {"Live", "Setup"};
static const int drum_main_targets[] = {M_DRUM_LIVE, M_DRUM_SETUP};

const machine_ui_t drum_menu_ui = {
    .main_items = drum_main_items,
    .main_targets = drum_main_targets,
    .n_main = 2,
    .register_pages = drum_register_pages,
    .main_event = drum_main_event,
    .boot_target = M_DRUM_LIVE,
};
