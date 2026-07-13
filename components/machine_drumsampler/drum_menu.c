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
// hit flash cycles red -> green -> blue per trigger
static const color_t PAD_LIT[3] = {{230, 60, 50}, {40, 200, 90}, {60, 120, 240}};
static uint8_t s_flash_col[DR_PADS];

// the selected pad lives in dr.sel_pad: the Live encoder picks it, CV6/CV7
// perform it (the engine reads it), and the Pads editor + browser act on it
static int s_load_ret = M_DRUM_PADS;   // page the sample browser returns to

// shared sorted library list (sample_ram) — big cards hold >200 samples
static char (*s_samples)[24] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;

static void refresh_samples(void){
    s_n_samples = sample_list_recent(&s_samples);   // 512, newest first — as everywhere
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], dr.pad[dr.sel_pad].sample) == 0) { s_sample_idx = i; break; }
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
    int r = (dr.n_pads <= 4) ? 9 : 6;
    color_t c = on ? PAD_LIT[s_flash_col[i] % 3]
                   : (dr.pad[i].len ? PAD_IDLE : PAD_EMPTY);
    TFT_fillCircle(cx, cy, r, c);
    s_lit[i] = on;
}

// the peak thumbnail (built in RAM at load, dr.pad[].wf). WHITE — unlike the
// sampler's bar there is no playhead to compete with (Arlo), and no transport
// bar either: a one-shot pad is over before a playhead would move. The band is
// handed the gap the text and the hit dot leave behind.
static void pad_waveform(int i, int bx, int by, int bw, int bh){
    dr_pad_t *p = &dr.pad[i];
    if (!p->wf_valid || bh < 6) return;
    for (int c = 0; c < bw - 1; c += 2){
        float a = sqrtf((float)p->wf[(c * DR_WF_W) / bw] / 255.0f);
        int ch = (int)(a * (float)bh);
        if (ch < 2) ch = 2;
        TFT_fillRect(bx + c, by + (bh - ch) / 2, 2, ch, WF_WHITE);
    }
}

static void draw_pad_cell(int i, bool lit){
    int x, y, w, h, cx, cy, num_left, num_top;
    pad_cell_rect(i, &x, &y, &w, &h);
    pad_corners(i, x, y, w, h, &cx, &cy, &num_left, &num_top);
    dr_pad_t *p = &dr.pad[i];
    bool sel = (i == dr.sel_pad);
    bool big = (dr.n_pads <= 4);
    _bg = p->len ? PAD_IDLE : PAD_EMPTY;    // static cell; the dot does the flashing
    TFT_fillRect(x, y, w, h, _bg);
    // the selected pad wears a BOLD white outline — it's what the encoder turns
    // pick, what CV6/CV7 perform, and what a press loads a sample into
    _fg = sel ? TFT_WHITE : (p->enabled ? (color_t){90, 90, 110} : (color_t){50, 50, 60});
    TFT_drawRect(x, y, w, h, _fg);
    if (sel){
        TFT_drawRect(x + 1, y + 1, w - 2, h - 2, _fg);
        TFT_drawRect(x + 2, y + 2, w - 4, h - 4, _fg);
    }

    char nm[10], n[16], src[16];
    snprintf(nm, sizeof(nm), "%.8s", p->sample[0] ? p->sample : "-");
    snprintf(n, sizeof(n), "%d", i + 1);
    if (dr.cv_select) snprintf(src, sizeof(src), "sel");
    else snprintf(src, sizeof(src), "CV%d", (p->trig_src & 7) + 1);   // 1..8, always
    Font f = cfont;

    // numeral + name on the edge opposite the dot: numeral in the outer corner,
    // name centred beside it
    TFT_setFont(big ? DEFAULT_FONT : DEF_SMALL_FONT, NULL);
    int fh = TFT_getfontheight();
    int ty = num_top ? y + 4 : y + h - fh - 4;
    _fg = p->enabled ? TFT_WHITE : (color_t){90, 90, 90};
    TFT_print(n, num_left ? x + 6 : x + w - TFT_getStringWidth(n) - 6, ty);
    _fg = p->len ? TFT_WHITE : (color_t){110, 110, 110};
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
    // The dot's row bounds it as well as the caption's: the dot bulges above the
    // small type it sits beside.
    if (big){
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
        pad_waveform(i, x + 5, top, w - 10, bot - top);
    }
    pad_dot(i, lit);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(SCREEN_BG);
    _bg = SCREEN_BG; _fg = TFT_WHITE;
    TFT_print("Drums", 6, 4);
    for (int i = 0; i < dr.n_pads; i++) draw_pad_cell(i, dr.pad[i].playing);
    _bg = SCREEN_BG;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    const char *hint = dr.cv_mod ? "turn:pad  press:load  hold:setup  knob6/7: level/decay"
                                 : "turn:pad  press:load  hold:setup";
    TFT_print((char*)hint, 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

// turn = pick the pad the outline (and CV6/CV7) follow; only the two cells that
// changed are repainted — a full grid repaint per detent strobes
static void live_select(int dir){
    int prev = dr.sel_pad;
    int n = dr.n_pads;
    dr.sel_pad = (prev + dir + n) % n;
    if (dr.sel_pad == prev) return;
    draw_pad_cell(prev, dr.pad[prev].playing);
    draw_pad_cell(dr.sel_pad, dr.pad[dr.sel_pad].playing);
}

static int drum_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU:
            if (dr.sel_pad >= dr.n_pads) dr.sel_pad = 0;
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
            break;
        case EV_FWD: live_select(+1); break;
        case EV_BWD: live_select(-1); break;
        case EV_SHORT_PRESS:                        // load a sample into the selection
            refresh_samples();
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
static const char *pads_labels[] = {"Pad", "Sample", "Trig In", "Level", "Pan",
                                    "Decay", "Knob7 CW", "Retrig", "Enabled"};
#define DR_PADS_N 9
// pad select, Knob7 CW and Enabled are click-to-advance; the rest click-to-edit
#define PADS_IS_TOGGLE(i) ((i) == 0 || (i) == 6 || (i) == 8)

static void pads_value_str(int i, char *v, size_t n){
    dr_pad_t *p = &dr.pad[dr.sel_pad];
    switch(i){
        case 0: snprintf(v, n, "%d / %d", dr.sel_pad + 1, dr.n_pads); break;
        case 1: snprintf(v, n, "%s", p->sample[0] ? p->sample : "(none)"); break;
        case 2: snprintf(v, n, "CV%d", p->trig_src + 1); break;
        // level reads as % of unity: 100 % is the sample as recorded, past that
        // the pad is driven ("drv" = into the soft clipper)
        case 3: {
            int pct = (int)p->level * 100 / DR_LEVEL_UNITY;
            if (p->level > DR_LEVEL_UNITY) snprintf(v, n, "%d%% drv", pct);
            else snprintf(v, n, "%d%%", pct);
            break;
        }
        case 4:
            if (p->pan == 128) snprintf(v, n, "C");
            else if (p->pan < 128) snprintf(v, n, "L%d", (128 - p->pan) * 100 / 128);
            else snprintf(v, n, "R%d", (p->pan - 128) * 100 / 127);
            break;
        // knob7 CCW writes decay; CW writes whichever target row 6 selects, so the
        // row shows the value that is actually live
        case 5:
            if (p->decay_ms == 0) snprintf(v, n, "FULL");
            else snprintf(v, n, "%dms", p->decay_ms);
            break;
        case 6:
            if (p->cw_mode == DR_CW_START)
                snprintf(v, n, "start %d%%", (int)p->start_off * 100 / 255);
            else if (p->cw_mode == DR_CW_ATTACK)
                snprintf(v, n, "attack %dms", p->attack_ms);
            else if (p->loop_ms)                       // named to match the Retrig row
                snprintf(v, n, "retrig %dms", p->loop_ms);
            else
                snprintf(v, n, "retrig");
            break;
        case 7:
            if (p->loop_reps == DR_REPS_INF) snprintf(v, n, "INF");
            else if (p->loop_reps) snprintf(v, n, "%d", p->loop_reps);
            else snprintf(v, n, "sample");     // run the loop out over the sample
            break;
        case 8: snprintf(v, n, "%s", p->enabled ? "ON" : "OFF"); break;
        default: v[0] = 0;
    }
}

static void pads_row_redraw(int i, int pos, int sel){
    char raw[24];
    pads_value_str(i, raw, sizeof(raw));
    row_draw(i, pos, sel, pads_labels[i], raw);
}

static void pads_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = SCREEN_BG; TFT_fillScreen(SCREEN_BG);
    _fg = TFT_WHITE;
    TFT_print("Drum Pads", 6, 4);
    for (int i = 0; i < DR_PADS_N; i++) pads_row_redraw(i, pos, sel);
    _bg = SCREEN_BG;
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press Sample to open the browser; hold to go back", 8,
              _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void pads_adj(int i, int dir){
    dr_pad_t *p = &dr.pad[dr.sel_pad];
    switch(i){
        case 0: dr.sel_pad = (dr.sel_pad + (dir > 0 ? 1 : dr.n_pads - 1)) % dr.n_pads; break;
        case 2: p->trig_src = (p->trig_src + (dir > 0 ? 1 : 7)) & 7; break;
        case 3: {
            int lv = (int)p->level + dir * 16;
            if (lv < 0) lv = 0;
            if (lv > DR_LEVEL_MAX) lv = DR_LEVEL_MAX;   // past unity = drive
            p->level = (uint16_t)lv;
            break;
        }
        case 4: {
            int pn = (int)p->pan + dir * 16;
            if (pn < 0) pn = 0;
            if (pn > 255) pn = 255;
            p->pan = (uint8_t)pn;
            break;
        }
        case 5: {
            int dm = (int)p->decay_ms + dir * 50;
            if (dm < 0) dm = 0;
            if (dm > 5000) dm = 5000;
            p->decay_ms = (uint16_t)dm;
            break;
        }
        // switching the CW target retires the others — leaving a stale attack (or
        // skipped head, or loop) applied to a pad whose knob no longer drives it
        // would be a setting nothing on screen explains
        case 6:
            p->cw_mode = (uint8_t)((p->cw_mode + 1) % DR_CW_MODES);
            p->attack_ms = 0;
            p->start_off = 0;
            p->loop_ms = 0;
            break;
        // the retrig ladder: run the loop out over the sample, a fixed count, or
        // INF — hold the stutter until the pad is hit again
        case 7: {
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
        case 8: p->enabled = !p->enabled; break;
    }
}

static int drum_pads_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: sel = 0; if (dr.sel_pad >= dr.n_pads) dr.sel_pad = 0; pads_redraw(pos, sel); break;
        case EV_FWD:
        case EV_BWD: {
            int dir = (event == EV_FWD) ? +1 : -1;
            if(sel){
                pads_adj(pos, dir);
                pads_row_redraw(pos, pos, sel);      // value edit: one row only
            } else {
                pos = (pos + dir + DR_PADS_N) % DR_PADS_N;
                pads_redraw(pos, sel);
            }
            break;
        }
        case EV_SHORT_PRESS:
            if(pos == 1){ refresh_samples(); s_load_ret = M_DRUM_PADS; return M_DRUM_LOAD; }
            if(PADS_IS_TOGGLE(pos)){
                pads_adj(pos, +1);                   // small option set: flip in place
                if (pos == 0) pads_redraw(pos, sel); // pad switch: every value changes
                else pads_row_redraw(pos, pos, 0);
            } else {
                sel = !sel; pads_row_redraw(pos, pos, sel);
            }
            break;
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
    snprintf(h, sizeof(h), "Pad %d  (%d/%d)", dr.sel_pad + 1, s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
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
            // re-loading the sample the pad already holds costs an SD read and
            // rebuilds the thumbnail for nothing (Arlo) — confirming is a no-op
            if(s_n_samples &&
               strcmp(s_samples[s_sample_idx], dr.pad[dr.sel_pad].sample) != 0)
                drum_load_pad(dr.sel_pad, s_samples[s_sample_idx]);
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
enum { R_PADEDIT = 0, R_PADS, R_TRIG, R_SENS, R_SEL1, R_SEL2, R_VEL, R_KNOB, R_COUNT };

static const char *setup_labels[R_COUNT] = {"Pad Setup", "Pads", "Trigger", "Sensi",
                                            "Sel CV TR1", "Sel CV TR2", "Velocity",
                                            "Knob 6/7"};
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
        case R_TRIG: snprintf(v, n, "%s", dr.cv_select ? "CV-select" : "Direct"); break;
        case R_SENS: snprintf(v, n, "%s", dr.sens == 0 ? "Low" : (dr.sens == 2 ? "High" : "Med")); break;
        case R_SEL1: sel_src_str(dr.sel_src[0], v, n); break;
        case R_SEL2: sel_src_str(dr.sel_src[1], v, n); break;
        case R_VEL:  snprintf(v, n, "%s", dr.velocity ? "ON" : "OFF"); break;
        case R_KNOB: snprintf(v, n, "%s", dr.cv_mod ? "level/decay" : "OFF"); break;
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
    const char *hint = dr.cv_select ? "gate on TR1/TR2 fires the pad its sel CV picks"
                                    : "each CV input fires its own pad (75% edge)";
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
        case R_TRIG: dr.cv_select = !dr.cv_select; break;
        case R_SENS: dr.sens = (dr.sens + (dir > 0 ? 1 : 2)) % 3; break;
        case R_SEL1: dr.sel_src[0] = sel_cycle(dr.sel_src[0], dir); break;
        case R_SEL2: dr.sel_src[1] = sel_cycle(dr.sel_src[1], dir); break;
        case R_VEL:  dr.velocity = !dr.velocity; break;
        case R_KNOB: dr.cv_mod = !dr.cv_mod; break;
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
        int loaded = 0;
        for (int i = 0; i < dr.n_pads; i++) if (dr.pad[i].len) loaded++;
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
