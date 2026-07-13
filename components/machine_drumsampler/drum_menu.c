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

// shared sorted library list (sample_ram) — big cards hold >200 samples
static char (*s_samples)[24] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;

static void refresh_samples(void){
    s_n_samples = sample_list_recent(&s_samples);   // 512, newest first — as everywhere
    s_sample_idx = 0;
    const char *cur = dr.pad[dr.sel_pad].ly[s_layer].sample;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], cur) == 0) { s_sample_idx = i; break; }
}

// ---- Live page: pad grid ----------------------------------------------------
static bool s_lit[DR_PADS];

static void pad_cell_rect(int i, int *x, int *y, int *w, int *h){
    int cols = (dr.n_pads <= 4) ? 2 : 4;    // 4 voices = 2x2, 8 = 4x2
    int rows = (dr.n_pads + cols - 1) / cols;
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
    int cols = (dr.n_pads <= 4) ? 2 : 4;
    int col = i % cols, row = i / cols;
    int r = (dr.n_pads <= 4) ? 9 : 6;
    bool dot_right = (col < cols / 2);      // dot hugs the screen centre...
    bool dot_bottom = (row == 0);
    *dot_cx = dot_right ? x + w - r - 5 : x + r + 5;
    *dot_cy = dot_bottom ? y + h - r - 5 : y + r + 5;
    *num_left = dot_right;                  // ...so the numeral takes the far corner
    *num_top  = dot_bottom;
}

static void pad_dot(int i, bool on){
    int x, y, w, h, cx, cy, nl, nt;
    pad_cell_rect(i, &x, &y, &w, &h);
    pad_corners(i, x, y, w, h, &cx, &cy, &nl, &nt);
    dr_pad_t *p = &dr.pad[i];
    int r = (dr.n_pads <= 4) ? 9 : 6;
    color_t idle = p->ly[0].len ? PAD_IDLE : PAD_EMPTY;
    // which layer is sounding shows in the dot's SHAPE, not another colour — the
    // R/G/B flash cycle has to survive (it's what makes a roll readable)
    if (on && dr.layers_on && p->cur == 1){
        TFT_fillCircle(cx, cy, r, idle);                       // B: a ring
        TFT_drawCircle(cx, cy, r, PAD_LIT[s_flash_col[i] % 3]);
        TFT_drawCircle(cx, cy, r - 1, PAD_LIT[s_flash_col[i] % 3]);
    } else {
        TFT_fillCircle(cx, cy, r, on ? PAD_LIT[s_flash_col[i] % 3] : idle);
    }
    s_lit[i] = on;
}

// the peak thumbnail (built in RAM at load, dr.pad[].wf). WHITE — unlike the
// sampler's bar there is no playhead to compete with (Arlo), and no transport
// bar either: a one-shot pad is over before a playhead would move. The band is
// handed the gap the text and the hit dot leave behind.
static void pad_waveform(int i, int bx, int by, int bw, int bh){
    dr_pad_t *p = &dr.pad[i];
    if (bh < 6) return;
    bool split = dr.layers_on && p->ly[1].wf_valid && p->ly[1].len;
    if (!split){                                  // exactly as before: A, centred
        if (!p->ly[0].wf_valid) return;
        for (int c = 0; c < bw - 1; c += 2){
            float a = sqrtf((float)p->ly[0].wf[(c * DR_WF_W) / bw] / 255.0f);
            int ch = (int)(a * (float)bh);
            if (ch < 2) ch = 2;
            TFT_fillRect(bx + c, by + (bh - ch) / 2, 2, ch, WF_WHITE);
        }
        return;
    }
    // two layers: A grows UP from the midline, B grows DOWN, in its own hue. No
    // extra text — the cell has none to spare.
    int mid = by + bh / 2;
    for (int c = 0; c < bw - 1; c += 2){
        if (p->ly[0].wf_valid){
            float a = sqrtf((float)p->ly[0].wf[(c * DR_WF_W) / bw] / 255.0f);
            int ch = (int)(a * (float)(mid - by));
            if (ch < 2) ch = 2;
            TFT_fillRect(bx + c, mid - ch, 2, ch, WF_WHITE);
        }
        float b = sqrtf((float)p->ly[1].wf[(c * DR_WF_W) / bw] / 255.0f);
        int chb = (int)(b * (float)(by + bh - mid));
        if (chb < 2) chb = 2;
        TFT_fillRect(bx + c, mid, 2, chb, WF_B);
    }
}

static void draw_pad_cell(int i, bool lit){
    int x, y, w, h, cx, cy, num_left, num_top;
    pad_cell_rect(i, &x, &y, &w, &h);
    pad_corners(i, x, y, w, h, &cx, &cy, &num_left, &num_top);
    dr_pad_t *p = &dr.pad[i];
    bool sel = (i == dr.sel_pad);
    bool big = (dr.n_pads <= 4);
    _bg = p->ly[0].len ? PAD_IDLE : PAD_EMPTY;  // static cell; the dot does the flashing
    TFT_fillRect(x, y, w, h, _bg);
    // the selected pad wears a BOLD white outline — it's what the encoder turns
    // pick, what CV6/CV7 perform, and what a press loads a sample into
    _fg = sel ? TFT_WHITE : (p->enabled ? (color_t){90, 90, 110} : (color_t){50, 50, 60});
    TFT_drawRect(x, y, w, h, _fg);
    if (sel){
        TFT_drawRect(x + 1, y + 1, w - 2, h - 2, _fg);
        TFT_drawRect(x + 2, y + 2, w - 4, h - 4, _fg);
    }

    char nm[10], n[16], src[16], src2[16];
    snprintf(nm, sizeof(nm), "%.8s", p->ly[0].sample[0] ? p->ly[0].sample : "-");
    snprintf(n, sizeof(n), "%d", i + 1);
    int sa = p->ly[0].trig_src, sb = p->ly[1].trig_src;
    if (dr.cv_select) snprintf(src, sizeof(src), "sel");
    else if (sa == DR_SRC_NONE) snprintf(src, sizeof(src), "--");
    else snprintf(src, sizeof(src), "CV%d", (sa & 7) + 1);            // 1..8, always
    // the B layer's own trigger, under A's, in B's hue. Only A's NAME is captioned;
    // B's lives on the Pads page (a name that changes with the last hit strobes).
    bool showb = dr.layers_on && p->ly[1].len;
    if (!showb)                 src2[0] = 0;
    else if (dr.cv_select)      snprintf(src2, sizeof(src2), "TR2");
    else if (sb == DR_SRC_NONE) snprintf(src2, sizeof(src2), "--");
    else snprintf(src2, sizeof(src2), "CV%d", (sb & 7) + 1);
    Font f = cfont;

    // numeral + name on the edge opposite the dot: numeral in the outer corner,
    // name centred beside it
    TFT_setFont(big ? DEFAULT_FONT : DEF_SMALL_FONT, NULL);
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
    int sy_a = src2[0] ? (num_top ? sy - sfh - 1 : sy) : sy;   // stack the two labels
    TFT_print(src, num_left ? x + 6 : x + w - TFT_getStringWidth(src) - 6, sy_a);
    if (src2[0]){
        _fg = WF_B;
        TFT_print(src2, num_left ? x + 6 : x + w - TFT_getStringWidth(src2) - 6,
                  num_top ? sy : sy + sfh + 1);
    }
    cfont = f;
    TFT_setFont(DEFAULT_FONT, NULL);

    // the waveform gets EVERYTHING left over — every pixel between the two text
    // rows, full cell width. It is the point of the cell; the text is a caption.
    // The dot's row bounds it as well as the caption's: the dot bulges above the
    // small type it sits beside.
    if (big){
        int r = 9;
        int dot_top = cy - r, dot_bot = cy + r;
        int top, bot;
        int lab_top = src2[0] ? sy_a : sy;              // the label block, both lines
        int lab_bot = src2[0] ? sy + sfh + 1 + sfh : sy + sfh;
        if (num_top){                                   // caption on top, dot below
            top = ty + fh + 3;
            bot = (lab_top < dot_top ? lab_top : dot_top) - 3;
        } else {                                        // dot on top, caption below
            int under = lab_bot > dot_bot ? lab_bot : dot_bot;
            top = under + 3;
            bot = ty - 3;
        }
        pad_waveform(i, x + 5, top, w - 10, bot - top);
    }
    pad_dot(i, lit);
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
    if (blocked)              { snprintf(s, n, "FLT sel"); return; }
    if (!dr.flt_on)           { snprintf(s, n, "FLT");     return; }
    if (dr.flt_mode == 0)     { snprintf(s, n, "FLT --");  return; }
    // the cutoff the knob is actually asking for, in the deck's own mapping
    int cv = dr.flt_cv;
    float fc;
    if (dr.flt_mode == 1) fc = 80.0f  * powf(150.0f, (float)cv / (2048.0f - 150.0f));
    else                  fc = 30.0f  * powf(200.0f, (float)(cv - 2198) / (4095.0f - 2198.0f));
    const char *m = (dr.flt_mode == 1) ? "LP" : "HP";
    if (fc >= 1000.0f) snprintf(s, n, "%s %.1fk", m, fc / 1000.0f);
    else               snprintf(s, n, "%s %d", m, (int)fc);
}

static void draw_filter_box(void){
    if (!dr.flt_box) return;
    bool sel = dr.sel_filter;
    int fh = TFT_getfontheight();
    int bh = fh + 8, bw = FBOX_W;
    int bx = _width - bw - 4, by = 2;
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
    for (int i = 0; i < dr.n_pads; i++) draw_pad_cell(i, dr.pad[i].playing);
    s_fbox_mode = -1;                      // force the box to paint
    draw_filter_box();
    _bg = SCREEN_BG;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    const char *hint = dr.sel_filter
        ? "turn:select  press:filter on/off  knob6:sweep  knob7:res"
        : (dr.cv_mod ? "turn:select  press:load  hold:setup  knob6/7: level/decay"
                     : "turn:select  press:load  hold:setup");
    TFT_print((char*)hint, 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

// turn = pick the pad (or the filter box) the outline and CV6/CV7 follow; only
// what changed is repainted — a full grid repaint per detent strobes
static void live_select(int dir){
    int n = dr.n_pads + (dr.flt_box ? 1 : 0);
    int prev = dr.sel_filter ? dr.n_pads : dr.sel_pad;
    int cur = (prev + dir + n) % n;
    if (cur == prev) return;
    int prev_pad = dr.sel_pad;
    if (cur == dr.n_pads){                     // landed on the filter box
        dr.sel_filter = true;
        dr.flt_take_f = dr.flt_take_q = false; // arm the knob pickup: the sweep
        dr.flt_ref_f = dr.flt_ref_q = -1;      // must not jump to where a knob sits
    } else {
        dr.sel_filter = false;
        dr.sel_pad = cur;
    }
    if (prev != dr.n_pads) draw_pad_cell(prev_pad, dr.pad[prev_pad].playing);
    if (!dr.sel_filter)    draw_pad_cell(dr.sel_pad, dr.pad[dr.sel_pad].playing);
    draw_filter_box();
}

static int drum_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            if (dr.sel_pad >= dr.n_pads) dr.sel_pad = 0;
            if (!dr.flt_box) dr.sel_filter = false;
            live_full_redraw();
            break;
        case EV_TIMER_REPEATING_FAST:
        case EV_TIMER_REPEATING_SLOW:
            for (int i = 0; i < dr.n_pads; i++){
                bool lit = dr.pad[i].playing;
                if (dr.pad[i].hit){
                    dr.pad[i].hit = false;
                    s_flash_col[i]++;               // next hit, next colour
                    pad_dot(i, true);               // dot only — fast enough to groove
                }
                else if (lit != s_lit[i]) pad_dot(i, lit);
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
            s_layer = 0;                            // a pad: load its A sample (B is
            refresh_samples();                      // a deliberate Pads-page act)
            s_load_ret = M_DRUM_LIVE;
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
enum { PR_PAD = 0, PR_LAYER, PR_SAMPLE, PR_TRIG, PR_LEVEL, PR_PAN, PR_DECAY,
       PR_CW, PR_RETRIG, PR_ENABLED, PR_COUNT };

static const char *pads_labels[PR_COUNT] = {"Pad", "Layer", "Sample", "Trig In",
                                            "Level", "Pan", "Decay", "Knob7 CW",
                                            "Retrig", "Enabled"};
// small option sets flip on a click; ranges keep click-to-edit
#define PADS_IS_TOGGLE(id) ((id) == PR_PAD || (id) == PR_LAYER || \
                            (id) == PR_CW  || (id) == PR_ENABLED)

static int s_prows[PR_COUNT], s_pnrows;

static void pads_build_rows(void){
    s_pnrows = 0;
    for (int id = 0; id < PR_COUNT; id++){
        if (id == PR_LAYER && !dr.layers_on) continue;
        s_prows[s_pnrows++] = id;
    }
    if (!dr.layers_on) s_layer = 0;
}

static void pads_value_str(int id, char *v, size_t n){
    dr_pad_t *p = &dr.pad[dr.sel_pad];
    dr_layer_t *L = &p->ly[s_layer];       // Sample + Trig In address the LAYER;
    switch(id){                            // everything else is the pad
        case PR_PAD:   snprintf(v, n, "%d / %d", dr.sel_pad + 1, dr.n_pads); break;
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
            if (p->cw_mode == DR_CW_START)
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
    TFT_print(dr.layers_on ? "B chokes A: one voice, two sounds"
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
            dr.sel_pad = (dr.sel_pad + (dir > 0 ? 1 : dr.n_pads - 1)) % dr.n_pads;
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
            if (dr.sel_pad >= dr.n_pads) dr.sel_pad = 0;
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
            if(id == PR_SAMPLE){ refresh_samples(); s_load_ret = M_DRUM_PADS; return M_DRUM_LOAD; }
            if(PADS_IS_TOGGLE(id)){
                pads_adj(id, +1);                    // small option set: flip in place
                // switching pad OR layer changes every value on the page
                if (id == PR_PAD || id == PR_LAYER) pads_redraw(pos, sel);
                else pads_row_redraw(pos, pos, 0);
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

// ---- Load browser (center-justified, slot -1 = clear) ------------------------
static void load_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(SCREEN_BG);
    int fh = TFT_getfontheight();
    _bg = SCREEN_BG; _fg = TFT_LIGHTGREY;
    char h[64];
    if (dr.layers_on)
        snprintf(h, sizeof(h), "Pad %d%c  (%d/%d)", dr.sel_pad + 1, s_layer ? 'B' : 'A',
                 s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
    else
        snprintf(h, sizeof(h), "Pad %d  (%d/%d)", dr.sel_pad + 1,
                 s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    if (!s_n_samples){
        char *m = "no samples in usr/";
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
        if (up >= 0){ char *n = s_samples[up]; TFT_print(n, _width / 2 - TFT_getStringWidth(n) / 2, yup); }
        if (dn < s_n_samples){ char *n = s_samples[dn]; TFT_print(n, _width / 2 - TFT_getStringWidth(n) / 2, ydn); }
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:browse  press:load  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int drum_load_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: load_redraw(); break;
        case EV_FWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + 1) % s_n_samples; load_redraw(); } break;
        case EV_BWD: if(s_n_samples){ s_sample_idx = (s_sample_idx + s_n_samples - 1) % s_n_samples; load_redraw(); } break;
        case EV_SHORT_PRESS:
            // re-loading the sample the layer already holds costs an SD read and
            // rebuilds the thumbnail for nothing (Arlo) — confirming is a no-op
            if(s_n_samples &&
               strcmp(s_samples[s_sample_idx], dr.pad[dr.sel_pad].ly[s_layer].sample) != 0)
                drum_load_layer(dr.sel_pad, s_layer, s_samples[s_sample_idx]);
            return s_load_ret;
        case EV_LONG_PRESS:
            return s_load_ret;       // cancel without loading
        default: break;
    }
    return 0;
}

// ---- Setup page ---------------------------------------------------------------
// Row PADEDIT opens the per-pad editor (Arlo: Pads belongs under Setup, not in
// a hub). The two selector-CV rows exist ONLY in CV-select mode — in Direct
// mode the selectors do nothing, so the page doesn't mention them at all and
// the encoder walks straight past.
enum { R_PADEDIT = 0, R_PADS, R_LAYERS, R_TRIG, R_SENS, R_SEL1, R_SEL2, R_VEL,
       R_KNOB, R_FILTER, R_COUNT };

static const char *setup_labels[R_COUNT] = {"Pad Setup", "Pads", "B Layers", "Trigger",
                                            "Sensi", "Sel CV TR1", "Sel CV TR2",
                                            "Velocity", "Knob 6/7", "Filter"};
// everything is a small option set except the two selector CVs (none + 8
// channels), which stay click-to-edit
#define SETUP_IS_TOGGLE(id) ((id) != R_PADEDIT && (id) != R_SEL1 && (id) != R_SEL2)

// visible rows, rebuilt whenever the trigger mode changes; `pos` indexes THIS
static int s_rows[R_COUNT], s_nrows;

static void setup_build_rows(void){
    s_nrows = 0;
    for (int id = 0; id < R_COUNT; id++){
        if ((id == R_SEL1 || id == R_SEL2) && !dr.cv_select) continue;
        s_rows[s_nrows++] = id;
    }
}

static void sel_src_str(int src, char *v, size_t n){
    if (src < 0) snprintf(v, n, "none");
    else snprintf(v, n, "CV%d", (src & 7) + 1);
}

static void setup_value_str(int id, char *v, size_t n){
    switch(id){
        case R_PADEDIT: v[0] = 0; break;      // a page link: no value to show
        case R_PADS: snprintf(v, n, "%d", dr.n_pads); break;
        case R_LAYERS: snprintf(v, n, "%s", dr.layers_on ? "A+B choke" : "OFF"); break;
        case R_TRIG: snprintf(v, n, "%s", dr.cv_select ? "CV-select" : "Direct"); break;
        case R_SENS: snprintf(v, n, "%s", dr.sens == 0 ? "Low" : (dr.sens == 2 ? "High" : "Med")); break;
        case R_SEL1: sel_src_str(dr.sel_src[0], v, n); break;
        case R_SEL2: sel_src_str(dr.sel_src[1], v, n); break;
        case R_VEL:  snprintf(v, n, "%s", dr.velocity ? "ON" : "OFF"); break;
        case R_KNOB: snprintf(v, n, "%s", dr.cv_mod ? "level/decay" : "OFF"); break;
        case R_FILTER: snprintf(v, n, "%s", dr.flt_box ? "box" : "OFF"); break;
        default: v[0] = 0;
    }
}

static void setup_row_redraw(int idx, int pos, int sel){
    char raw[24];
    int id = s_rows[idx];
    setup_value_str(id, raw, sizeof(raw));
    row_draw(idx, pos, sel, setup_labels[id], raw);
}

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = SCREEN_BG; TFT_fillScreen(SCREEN_BG);
    _fg = TFT_WHITE;
    TFT_print("Drum Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);   // top-right; pos -1 = System
    for (int i = 0; i < s_nrows; i++) setup_row_redraw(i, pos, sel);
    _bg = SCREEN_BG;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    const char *hint = dr.layers_on
        ? (dr.cv_select ? "TR1 fires A, TR2 fires B — of the pad the sel CV picks"
                        : "each pad's B sample chokes its A (one voice)")
        : (dr.cv_select ? "gate on TR1/TR2 fires the pad its sel CV picks"
                        : "each CV input fires its own pad (75% edge)");
    TFT_print((char*)hint, 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
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
        case R_PADS: dr.n_pads = (dr.n_pads == 8) ? 4 : 8; if (dr.sel_pad >= dr.n_pads) dr.sel_pad = 0; break;
        case R_LAYERS: dr.layers_on = !dr.layers_on; break;
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
    }
}

static int drum_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_build_rows(); setup_redraw(pos, sel); break;
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if(sel){
                setup_adj(s_rows[pos], dir);
                setup_row_redraw(pos, pos, sel);              // value edit: one row only
            } else {
                pos += dir;
                if(pos >= s_nrows) pos = -1;                  // past bottom -> System
                if(pos < -1) pos = s_nrows - 1;               // past System -> bottom
                setup_redraw(pos, sel);
            }
            break;
        }
        case EV_SHORT_PRESS: {
            if(pos == -1) return M_MORE;                       // System affordance
            int id = s_rows[pos];
            if(id == R_PADEDIT) return M_DRUM_PADS;            // per-pad editor
            if(SETUP_IS_TOGGLE(id)){
                setup_adj(id, +1);                             // small option set: flip here
                if (id == R_TRIG){
                    // the mode adds/removes the selector rows and rewrites the
                    // hint line: rebuild the page around the row we're still on
                    setup_build_rows();
                    if (pos >= s_nrows) pos = s_nrows - 1;
                    setup_redraw(pos, sel);
                } else setup_row_redraw(pos, pos, 0);
            } else {
                sel = !sel; setup_row_redraw(pos, pos, sel);
            }
            break;
        }
        case EV_LONG_PRESS: return M_DRUM_LIVE;   // toggle Setup -> Live (no hub)
        default: break;
    }
    return 0;
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
        int nly = dr.layers_on ? DR_LAYERS : 1;
        for (int i = 0; i < dr.n_pads; i++)
            for (int l = 0; l < nly; l++) if (dr.pad[i].ly[l].len) loaded++;
        char s[64];
        snprintf(s, sizeof(s), "Drums: %d pads (%d loaded) %s",
                 dr.n_pads, loaded, dr.cv_select ? "cv-sel" : "direct");
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
