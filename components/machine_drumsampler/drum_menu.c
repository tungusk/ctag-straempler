// Drum sampler UI — Live pad grid (cells flash on trigger), a per-pad editor
// (sample / level / pan / enabled), a sample-load browser, and a Setup page
// (pad count / trigger mode / selector CVs / velocity).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "sample_ram.h"
#include "drum_priv.h"

static const color_t PAD_IDLE  = {36, 40, 52};
static const color_t PAD_EMPTY = {18, 20, 26};
// hit flash cycles red -> green -> blue per trigger
static const color_t PAD_LIT[3] = {{230, 60, 50}, {40, 200, 90}, {60, 120, 240}};
static uint8_t s_flash_col[DR_PADS];

static int s_pad = 0;               // pad being edited (Pads page + Load browser)

// shared sorted library list (sample_ram) — big cards hold >200 samples
static char (*s_samples)[24] = NULL;
static int  s_n_samples = 0, s_sample_idx = 0;

static void refresh_samples(void){
    s_n_samples = sample_list_shared(&s_samples);
    s_sample_idx = 0;
    for (int i = 0; i < s_n_samples; i++)
        if (strcmp(s_samples[i], dr.pad[s_pad].sample) == 0) { s_sample_idx = i; break; }
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

// flash indicator: a small dot at the cell corner facing screen centre —
// repainting the whole (large) cell per hit couldn't keep up with the rhythm
static void pad_dot(int i, bool on){
    int x, y, w, h;
    pad_cell_rect(i, &x, &y, &w, &h);
    int cols = (dr.n_pads <= 4) ? 2 : 4;
    int col = i % cols, row = i / cols;
    int r = (dr.n_pads <= 4) ? 9 : 6;
    int cx = (col < cols / 2) ? x + w - r - 5 : x + r + 5;
    int cy = (row == 0) ? y + h - r - 5 : y + r + 5;
    color_t c = on ? PAD_LIT[s_flash_col[i] % 3]
                   : (dr.pad[i].len ? PAD_IDLE : PAD_EMPTY);
    TFT_fillCircle(cx, cy, r, c);
    s_lit[i] = on;
}

static void draw_pad_cell(int i, bool lit){
    int x, y, w, h;
    pad_cell_rect(i, &x, &y, &w, &h);
    dr_pad_t *p = &dr.pad[i];
    _bg = p->len ? PAD_IDLE : PAD_EMPTY;    // static cell; the dot does the flashing
    TFT_fillRect(x, y, w, h, _bg);
    _fg = p->enabled ? TFT_WHITE : (color_t){90, 90, 90};
    TFT_drawRect(x, y, w, h, _fg);
    char n[16];
    snprintf(n, sizeof(n), "%d", i + 1);
    TFT_print(n, x + 4, y + 3);
    TFT_setFont(DEF_SMALL_FONT, NULL);
    char nm[10];
    snprintf(nm, sizeof(nm), "%.8s", p->sample[0] ? p->sample : "-");
    TFT_print(nm, x + 4, y + h - TFT_getfontheight() - 3);
    TFT_setFont(DEFAULT_FONT, NULL);
    pad_dot(i, lit);
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Drums", 6, 4);
    for (int i = 0; i < dr.n_pads; i++) draw_pad_cell(i, dr.pad[i].playing);
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    const char *hint = dr.cv_select ? "TR1/TR2 fire pad picked by sel CV"
                                    : "CV1-8 trigger pads 1-8";
    TFT_print((char*)hint, 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int drum_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
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
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Pads page: per-pad editor ----------------------------------------------
static const char *pads_labels[] = {"Pad", "Sample", "Trig In", "Level", "Pan", "Decay", "Enabled"};
#define DR_PADS_N 7

static void pads_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Drum Pads", 6, 4);
    dr_pad_t *p = &dr.pad[s_pad];
    for (int i = 0; i < DR_PADS_N; i++){
        int y = fh + 16 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)pads_labels[i], 8, y);
        char v[24];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%d / %d", s_pad + 1, dr.n_pads); break;
            case 1: snprintf(v, sizeof(v), "%s", p->sample[0] ? p->sample : "(none)"); break;
            case 2: snprintf(v, sizeof(v), "CV%d", p->trig_src + 1); break;
            case 3: snprintf(v, sizeof(v), "%d", p->level); break;
            case 4:
                if (p->pan == 128) snprintf(v, sizeof(v), "C");
                else if (p->pan < 128) snprintf(v, sizeof(v), "L%d", (128 - p->pan) * 100 / 128);
                else snprintf(v, sizeof(v), "R%d", (p->pan - 128) * 100 / 127);
                break;
            case 5:
                if (p->decay_ms == 0) snprintf(v, sizeof(v), "FULL");
                else snprintf(v, sizeof(v), "%dms", p->decay_ms);
                break;
            case 6: snprintf(v, sizeof(v), "%s", p->enabled ? "ON" : "OFF"); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("press Sample to open the browser", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void pads_adj(int i, int dir){
    dr_pad_t *p = &dr.pad[s_pad];
    switch(i){
        case 0: s_pad = (s_pad + (dir > 0 ? 1 : dr.n_pads - 1)) % dr.n_pads; break;
        case 2: p->trig_src = (p->trig_src + (dir > 0 ? 1 : 7)) & 7; break;
        case 3: {
            int lv = (int)p->level + dir * 16;
            if (lv < 0) lv = 0;
            if (lv > 255) lv = 255;
            p->level = (uint8_t)lv;
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
        case 6: p->enabled = !p->enabled; break;
    }
}

static int drum_pads_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: sel = 0; if (s_pad >= dr.n_pads) s_pad = 0; pads_redraw(pos, sel); break;
        case EV_FWD:
            if(sel) pads_adj(pos, +1);
            else pos = (pos + 1) % DR_PADS_N;
            pads_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel) pads_adj(pos, -1);
            else pos = (pos + DR_PADS_N - 1) % DR_PADS_N;
            pads_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == 1){ refresh_samples(); return M_DRUM_LOAD; }
            sel = !sel; pads_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_MAIN;
        default: break;
    }
    return 0;
}

// ---- Load browser (center-justified, slot -1 = clear) ------------------------
static void load_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[64];
    snprintf(h, sizeof(h), "Pad %d  (%d/%d)", s_pad + 1, s_n_samples ? s_sample_idx + 1 : 0, s_n_samples);
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
            if(s_n_samples) drum_load_pad(s_pad, s_samples[s_sample_idx]);
            return M_DRUM_PADS;
        case EV_LONG_PRESS:
            return M_DRUM_PADS;      // cancel without loading
        default: break;
    }
    return 0;
}

// ---- Setup page ---------------------------------------------------------------
static const char *setup_labels[] = {"Pads", "Trigger", "Sensi", "Sel CV TR1", "Sel CV TR2", "Velocity"};
#define DR_SETUP_N 6

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Drum Setup", 6, 4);
    for (int i = 0; i < DR_SETUP_N; i++){
        int y = fh + 16 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[24];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%d", dr.n_pads); break;
            case 1: snprintf(v, sizeof(v), "%s", dr.cv_select ? "CV-select" : "Direct"); break;
            case 2: snprintf(v, sizeof(v), "%s", dr.sens == 0 ? "Low" : (dr.sens == 2 ? "High" : "Med")); break;
            case 3: snprintf(v, sizeof(v), "CV%d", dr.sel_src[0] + 1); break;
            case 4: snprintf(v, sizeof(v), "CV%d", dr.sel_src[1] + 1); break;
            case 5: snprintf(v, sizeof(v), "%s", dr.velocity ? "ON" : "OFF"); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    const char *hint = dr.cv_select ? "gate on TR1/TR2 fires the pad its sel CV picks"
                                    : "each CV input fires its own pad (75% edge)";
    TFT_print((char*)hint, 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void setup_adj(int i, int dir){
    switch(i){
        case 0: dr.n_pads = (dr.n_pads == 8) ? 4 : 8; if (s_pad >= dr.n_pads) s_pad = 0; break;
        case 1: dr.cv_select = !dr.cv_select; break;
        case 2: dr.sens = (dr.sens + (dir > 0 ? 1 : 2)) % 3; break;
        case 3: dr.sel_src[0] = (dr.sel_src[0] + (dir > 0 ? 1 : 7)) & 7; break;
        case 4: dr.sel_src[1] = (dr.sel_src[1] + (dir > 0 ? 1 : 7)) & 7; break;
        case 5: dr.velocity = !dr.velocity; break;
    }
}

static int drum_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if(sel) setup_adj(pos, +1);
            else pos = (pos + 1) % DR_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel) setup_adj(pos, -1);
            else pos = (pos + DR_SETUP_N - 1) % DR_SETUP_N;
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS: sel = !sel; setup_redraw(pos, sel); break;
        case EV_LONG_PRESS: return M_MAIN;
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

static const char *const drum_main_items[] = {"Live", "Pads", "Setup"};
static const int drum_main_targets[] = {M_DRUM_LIVE, M_DRUM_PADS, M_DRUM_SETUP};

const machine_ui_t drum_menu_ui = {
    .main_items = drum_main_items,
    .main_targets = drum_main_targets,
    .n_main = 3,
    .register_pages = drum_register_pages,
    .main_event = drum_main_event,
};
