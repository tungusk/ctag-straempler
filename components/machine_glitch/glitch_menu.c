// M5 glitch UI — Live status view (big LIVE/GLITCH state + params) and a
// Setup page (Window / Reverse). No sample browser — it works on line-in.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "clock.h"
#include "beatlisten.h"
#include "menu_config.h"
#include "glitch_priv.h"

static const color_t LIVE_COL   = {40, 160, 90};
static const color_t GLITCH_COL = {230, 90, 40};

static bool s_last_stutter = false;
static int  s_last_win = -1;

static void state_block(void){
    bool st = gl.stutter;
    color_t c = st ? GLITCH_COL : LIVE_COL;
    int fh = TFT_getfontheight();
    int by = fh + 20, bh = 70;
    _bg = (color_t){ (uint8_t)(c.r/5), (uint8_t)(c.g/5), (uint8_t)(c.b/5) };
    TFT_fillRect(10, by, _width - 20, bh, _bg);
    _fg = c;
    TFT_drawRect(10, by, _width - 20, bh, _fg);
    Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
    _fg = st ? GLITCH_COL : LIVE_COL;
    const char *w = st ? "GLITCH" : "LIVE";
    TFT_print((char*)w, _width/2 - TFT_getStringWidth((char*)w)/2, by + bh/2 - TFT_getfontheight()/2);
    cfont = f;
    s_last_stutter = st;
}

static const char *div_name(int d){
    switch(d){ case 0: return "1/4"; case 1: return "1/8"; case 2: return "1/16"; default: return "1/32"; }
}

static void info_block(void){
    int fh = TFT_getfontheight();
    int y = fh + 100;
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, (fh + 4) * 2, _bg);
    _fg = TFT_WHITE;
    char s[48];
    snprintf(s, sizeof(s), "window %d ms   %s%s",
             gl.win_ms, gl.reverse ? "REV " : "", gl.latch ? "LATCH" : "");
    TFT_print(s, _width/2 - TFT_getStringWidth(s)/2, y + 2);
    char t[40];
    if (gl.sync){
        if (gl.ci.clk.locked) snprintf(t, sizeof(t), "SYNC %s  %.1f BPM", div_name(gl.division), gl.ci.clk.bpm);
        else               snprintf(t, sizeof(t), "SYNC %s  (no clock)", div_name(gl.division));
        _fg = gl.ci.clk.locked ? (color_t){40,200,90} : TFT_LIGHTGREY;
        TFT_print(t, _width/2 - TFT_getStringWidth(t)/2, y + fh + 6);
    }
    s_last_win = gl.win_ms;
}

static void live_full_redraw(void){
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Glitch", 6, 4);
    state_block();
    info_block();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("TR1:hold-glitch  TR2:latch  knob6:window  knob7:pitch", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int glitch_live_handler(int it_id, int event, void *ev_data){
    switch(event){
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            if (gl.stutter != s_last_stutter) state_block();
            info_block();   // refresh window/BPM/lock
            break;
        case EV_LONG_PRESS: return M_GLITCH_SETUP;   // toggle Live -> Setup
        default: break;
    }
    return 0;
}

// ---- Setup ----------------------------------------------------------------
static const char *setup_labels[] = {"Window ms", "Reverse", "Sync", "Division", "Clock Src"};
#define GL_SETUP_N 5

static void setup_redraw(int pos, int sel){
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE;
    TFT_print("Glitch Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);
    for (int i = 0; i < GL_SETUP_N; i++){
        int y = fh + 16 + i * (fh + 8);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 6, _bg);
        TFT_print((char*)setup_labels[i], 8, y);
        char v[24];
        switch(i){
            case 0: snprintf(v, sizeof(v), "%d", gl.win_ms); break;
            case 1: snprintf(v, sizeof(v), "%s", gl.reverse ? "ON" : "OFF"); break;
            case 2: snprintf(v, sizeof(v), "%s", gl.sync ? "ON" : "OFF"); break;
            case 3: snprintf(v, sizeof(v), "%s", div_name(gl.division)); break;
            case 4: snprintf(v, sizeof(v), "%s", clock_source_name(gl.clk_src)); break;
        }
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("window on knob6 (free) or clock division (sync)", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void gl_adj(int i, int dir){
    switch(i){
        case 0: gl.win_ms += dir * 10; if(gl.win_ms < 20) gl.win_ms = 20; if(gl.win_ms > 500) gl.win_ms = 500; break;
        case 1: gl.reverse = !gl.reverse; break;
        case 2: gl.sync = !gl.sync; break;
        case 3: gl.division += dir; if(gl.division < 0) gl.division = 0; if(gl.division > 3) gl.division = 3; break;
        case 4:
            // CV1..8 + AUDIO (both trigs are stutter controls); AUDIO wakes the ear
            gl.clk_src = clock_source_cycle_cv_audio(gl.clk_src, dir);
            if (gl.clk_src == CLK_SRC_AUDIO && beatlisten_get_mode() == BL_OFF) {
                beatlisten_set_mode(BL_GROOVE);
                configSetIntSetting("blisten", BL_GROOVE);
            }
            break;
    }
}

static int glitch_setup_handler(int it_id, int event, void *ev_data){
    static int pos = 0, sel = 0;
    switch(event){
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if(sel) gl_adj(pos, +1);
            else { pos++; if(pos >= GL_SETUP_N) pos = -1; }
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if(sel) gl_adj(pos, -1);
            else { pos--; if(pos < -1) pos = GL_SETUP_N - 1; }
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if(pos == -1) return M_MORE;   // System affordance
            sel = !sel; setup_redraw(pos, sel); break;
        case EV_LONG_PRESS: return M_GLITCH_LIVE;   // toggle Setup -> Live
        default: break;
    }
    return 0;
}

// ---- registration ---------------------------------------------------------
static void glitch_register_pages(void *menusys){
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_GLITCH_LIVE);  menusys_item_set_default_cb(_ms, M_GLITCH_LIVE, glitch_live_handler);
    menusys_new_item(_ms, M_GLITCH_SETUP); menusys_item_set_default_cb(_ms, M_GLITCH_SETUP, glitch_setup_handler);
}

static int glitch_main_event(int event, void *ev_data){
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW){
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        _fg = gl.stutter ? GLITCH_COL : LIVE_COL;
        char s[40];
        snprintf(s, sizeof(s), "Glitch: %s  win %dms", gl.stutter ? "GLITCH" : "live", gl.win_ms);
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const glitch_main_items[] = {"Live", "Setup"};
static const int glitch_main_targets[] = {M_GLITCH_LIVE, M_GLITCH_SETUP};

const machine_ui_t glitch_menu_ui = {
    .main_items = glitch_main_items,
    .main_targets = glitch_main_targets,
    .n_main = 2,
    .register_pages = glitch_register_pages,
    .main_event = glitch_main_event,
    .boot_target = M_GLITCH_LIVE,
};
