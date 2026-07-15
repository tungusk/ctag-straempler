// Synth UI — Live (note + gate + knob readout) and Setup (shape / tune / ADSR).
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include "machine.h"
#include "synth_priv.h"

static const color_t GATE_ON  = {40, 200, 90};
static const color_t GATE_OFF = {90, 90, 100};

static const char *const NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void note_name(float freq, char *buf, size_t n)
{
    if (freq < 1.0f) { snprintf(buf, n, "--"); return; }
    int midi = (int)lroundf(69.0f + 12.0f * log2f(freq / 440.0f));
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    snprintf(buf, n, "%s%d", NOTE_NAMES[midi % 12], midi / 12 - 1);
}

static bool s_last_gate;

static void live_gate_block(void)
{
    color_t c = sy.gate ? GATE_ON : GATE_OFF;
    int fh = TFT_getfontheight();
    int by = fh + 18, bh = 62;
    _bg = (color_t){ (uint8_t)(c.r / 6), (uint8_t)(c.g / 6), (uint8_t)(c.b / 6) };
    TFT_fillRect(10, by, _width - 20, bh, _bg);
    _fg = c; TFT_drawRect(10, by, _width - 20, bh, _fg);
    char nm[12]; note_name(sy.freq, nm, sizeof(nm));
    Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
    _fg = TFT_WHITE;
    TFT_print(nm, _width / 2 - TFT_getStringWidth(nm) / 2, by + bh / 2 - TFT_getfontheight() / 2);
    cfont = f;
    s_last_gate = sy.gate;
}

static void live_info(void)
{
    int fh = TFT_getfontheight();
    int y = fh + 90;
    _bg = TFT_BLACK; TFT_fillRect(0, y, _width, (fh + 6) * 2, _bg);
    char s[48];
    snprintf(s, sizeof(s), "%.0f Hz   cut %.0f   res %.0f%%",
             sy.freq, sy.cutoff_base, sy.res01 * 100.0f);
    _fg = TFT_LIGHTGREY;
    TFT_print(s, _width / 2 - TFT_getStringWidth(s) / 2, y);
    char t[48];
    snprintf(t, sizeof(t), "shape %.0f%%  %s", sy.shape * 100.0f, sy.quantize ? "quantized" : "free");
    TFT_print(t, _width / 2 - TFT_getStringWidth(t) / 2, y + fh + 4);
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Synth", 6, 4);
    live_gate_block();
    live_info();
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("CV1:pitch(1V/oct)  TR1:gate  knob6:cutoff  knob7:res", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static int synth_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU: live_full_redraw(); break;
        case EV_TIMER_REPEATING_SLOW:
        case EV_TIMER_REPEATING_FAST:
            if (sy.gate != s_last_gate) live_gate_block();
            live_info();
            break;
        case EV_LONG_PRESS: return M_SYNTH_SETUP;
        default: break;
    }
    return 0;
}

// ---- Setup -----------------------------------------------------------------
static const char *setup_labels[] = {
    "Shape", "Base Note", "Quantize", "Attack", "Decay", "Sustain", "Release", "Env>Cut", "Level"
};
#define SY_SETUP_N 9

static void setup_val(int i, char *v, size_t n)
{
    switch (i) {
        case 0: snprintf(v, n, "%.0f%%", sy.shape * 100.0f); break;
        case 1: { char nm[12]; note_name(440.0f * powf(2.0f, (sy.base_note - 69) / 12.0f), nm, sizeof(nm));
                  snprintf(v, n, "%s (%d)", nm, sy.base_note); break; }
        case 2: snprintf(v, n, "%s", sy.quantize ? "ON" : "OFF"); break;
        case 3: snprintf(v, n, "%d ms", (int)(sy.atk * 1000.0f)); break;
        case 4: snprintf(v, n, "%d ms", (int)(sy.dec * 1000.0f)); break;
        case 5: snprintf(v, n, "%.0f%%", sy.sus * 100.0f); break;
        case 6: snprintf(v, n, "%d ms", (int)(sy.rel * 1000.0f)); break;
        case 7: snprintf(v, n, "%.0f%%", sy.env_to_cut * 100.0f); break;
        case 8: snprintf(v, n, "%.0f%%", sy.level * 100.0f); break;
    }
}

static void setup_redraw(int pos, int sel)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Synth Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);
    for (int i = 0; i < SY_SETUP_N; i++) {
        int y = fh + 14 + i * (fh + 5);
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        TFT_print((char *)setup_labels[i], 8, y);
        char v[24]; setup_val(i, v, sizeof(v));
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("polyBLEP saw<->square; TR1 gates the ADSR", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void sy_adj(int i, int dir)
{
    float d = (float)dir;
    switch (i) {
        case 0: sy.shape += d * 0.05f; if (sy.shape < 0) sy.shape = 0; if (sy.shape > 1) sy.shape = 1; break;
        case 1: sy.base_note += dir; if (sy.base_note < 12) sy.base_note = 12; if (sy.base_note > 96) sy.base_note = 96; break;
        case 2: sy.quantize = !sy.quantize; break;
        case 3: sy.atk += d * 0.005f; if (sy.atk < 0.0005f) sy.atk = 0.0005f; if (sy.atk > 2) sy.atk = 2; break;
        case 4: sy.dec += d * 0.01f;  if (sy.dec < 0.001f) sy.dec = 0.001f; if (sy.dec > 2) sy.dec = 2; break;
        case 5: sy.sus += d * 0.05f;  if (sy.sus < 0) sy.sus = 0; if (sy.sus > 1) sy.sus = 1; break;
        case 6: sy.rel += d * 0.02f;  if (sy.rel < 0.001f) sy.rel = 0.001f; if (sy.rel > 3) sy.rel = 3; break;
        case 7: sy.env_to_cut += d * 0.05f; if (sy.env_to_cut < 0) sy.env_to_cut = 0; if (sy.env_to_cut > 1) sy.env_to_cut = 1; break;
        case 8: sy.level += d * 0.05f; if (sy.level < 0) sy.level = 0; if (sy.level > 1) sy.level = 1; break;
    }
}

static int synth_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    static int pos = 0, sel = 0;
    switch (event) {
        case EV_ENTERED_MENU: pos = 0; sel = 0; setup_redraw(pos, sel); break;
        case EV_FWD:
            if (sel) sy_adj(pos, +1);
            else { pos++; if (pos >= SY_SETUP_N) pos = -1; }
            setup_redraw(pos, sel);
            break;
        case EV_BWD:
            if (sel) sy_adj(pos, -1);
            else { pos--; if (pos < -1) pos = SY_SETUP_N - 1; }
            setup_redraw(pos, sel);
            break;
        case EV_SHORT_PRESS:
            if (pos == -1) return M_MORE;
            sel = !sel; setup_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_SYNTH_LIVE;
        default: break;
    }
    return 0;
}

static void synth_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_SYNTH_LIVE);  menusys_item_set_default_cb(_ms, M_SYNTH_LIVE, synth_live_handler);
    menusys_new_item(_ms, M_SYNTH_SETUP); menusys_item_set_default_cb(_ms, M_SYNTH_SETUP, synth_setup_handler);
}

static int synth_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK; TFT_fillRect(0, fh + 12, _width, 24, _bg);
        char nm[12]; note_name(sy.freq, nm, sizeof(nm));
        _fg = sy.gate ? GATE_ON : TFT_LIGHTGREY;
        char s[40]; snprintf(s, sizeof(s), "Synth: %s %s", nm, sy.gate ? "GATE" : "");
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

static const char *const synth_main_items[] = { "Live", "Setup" };
static const int synth_main_targets[] = { M_SYNTH_LIVE, M_SYNTH_SETUP };

const machine_ui_t synth_menu_ui = {
    .main_items = synth_main_items,
    .main_targets = synth_main_targets,
    .n_main = 2,
    .register_pages = synth_register_pages,
    .main_event = synth_main_event,
    .boot_target = M_SYNTH_LIVE,
};
