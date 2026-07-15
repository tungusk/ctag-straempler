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
#include "sample_browser.h"
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
    char t[56];
    if (sy.engine == ENG_FM)
        snprintf(t, sizeof(t), "FM  ratio %.2f  idx %.1f  %s", sy.fm_ratio, sy.fm_index, sy.quantize ? "quant" : "free");
    else if (sy.engine == ENG_WT)
        snprintf(t, sizeof(t), "WT  %s  %s", sy.wave_name[0] ? sy.wave_name : "(no wave)", sy.quantize ? "quant" : "free");
    else
        snprintf(t, sizeof(t), "VA  shape %.0f%%  %s", sy.shape * 100.0f, sy.quantize ? "quant" : "free");
    // no-wave cue: WT engine with nothing loaded reads RED, not grey
    if (sy.engine == ENG_WT && !sy.wave_name[0]) _fg = (color_t){220, 80, 60};
    else _fg = TFT_LIGHTGREY;
    TFT_print(t, _width / 2 - TFT_getStringWidth(t) / 2, y + fh + 4);
}

// live envelope meter — a moving bar so you SEE the voice breathing
static int s_last_env = -1;
static void live_meters(bool full)
{
    int fh = TFT_getfontheight();
    int x = 20, w = _width - 40, h = 12, y = fh + 122;
    if (full) {
        TFT_drawRect(x, y, w, h, (color_t){40, 60, 110});
        _fg = (color_t){90, 90, 90}; _bg = TFT_BLACK;
        TFT_print("env", x, y - fh - 2);
        s_last_env = -1;
    }
    int fill = (int)(sy.env * (float)(w - 2));
    if (fill != s_last_env) {
        s_last_env = fill;
        if (fill > 0)     TFT_fillRect(x + 1, y + 1, fill, h - 2, (color_t){40, 200, 90});
        if (fill < w - 2) TFT_fillRect(x + 1 + fill, y + 1, (w - 2) - fill, h - 2, (color_t){18, 18, 24});
    }
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Synth", 6, 4);
    live_gate_block();
    live_info();
    live_meters(true);
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
            live_meters(false);
            break;
        case EV_LONG_PRESS: return M_SYNTH_SETUP;
        default: break;
    }
    return 0;
}

// ---- Setup -----------------------------------------------------------------
static const char *setup_labels[] = {
    "Engine", "Base Note", "Quantize", "Shape", "FM Ratio", "FM Index",
    "Attack", "Decay", "Sustain", "Release", "Env>Cut", "Glide",
    "LFO Rate", "LFO Depth", "LFO Dest", "Level", "Reverb", "Rev Mix", "Load Wave"
};
#define SY_SETUP_N 19
#define SY_LOAD_ITEM 18          // the "Load Wave" action row

static void setup_val(int i, char *v, size_t n)
{
    switch (i) {
        case 0: snprintf(v, n, "%s", sy.engine == ENG_FM ? "FM" : sy.engine == ENG_WT ? "WT" : "VA"); break;
        case 1: { char nm[12]; note_name(440.0f * powf(2.0f, (sy.base_note - 69) / 12.0f), nm, sizeof(nm));
                  snprintf(v, n, "%s (%d)", nm, sy.base_note); break; }
        case 2: snprintf(v, n, "%s", sy.quantize ? "ON" : "OFF"); break;
        case 3: snprintf(v, n, "%.0f%%", sy.shape * 100.0f); break;
        case 4: snprintf(v, n, "%.2f", sy.fm_ratio); break;
        case 5: snprintf(v, n, "%.1f", sy.fm_index); break;
        case 6: snprintf(v, n, "%d ms", (int)(sy.atk * 1000.0f)); break;
        case 7: snprintf(v, n, "%d ms", (int)(sy.dec * 1000.0f)); break;
        case 8: snprintf(v, n, "%.0f%%", sy.sus * 100.0f); break;
        case 9: snprintf(v, n, "%d ms", (int)(sy.rel * 1000.0f)); break;
        case 10: snprintf(v, n, "%.0f%%", sy.env_to_cut * 100.0f); break;
        case 11: snprintf(v, n, "%d ms", (int)(sy.glide * 1000.0f)); break;
        case 12: snprintf(v, n, "%.1f Hz", sy.lfo_rate); break;
        case 13: snprintf(v, n, "%.0f%%", sy.lfo_depth * 100.0f); break;
        case 14: snprintf(v, n, "%s", sy.lfo_dest == LFO_CUT ? "cutoff" : sy.lfo_dest == LFO_PITCH ? "pitch" : "off"); break;
        case 15: snprintf(v, n, "%.0f%%", sy.level * 100.0f); break;
        case 16: snprintf(v, n, "%s", reverb_mode_name(sy.rv.mode)); break;
        case 17: snprintf(v, n, "%.0f%%", sy.rv.wet * 100.0f); break;
        case 18: snprintf(v, n, "%s", sy.wave_name[0] ? sy.wave_name : "(none)"); break;
    }
}

static void setup_redraw(int pos, int sel)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print("Synth Setup", 6, 4);
    menuTFTPrintAffordance("Machine", pos == -1);
    // scrollable list — the param count exceeds the screen; keep the cursor in view
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (pos >= vis) top = pos - vis + 1;
    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= SY_SETUP_N) break;
        int y = y0 + r * row_h;
        _bg = (i == pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == pos && sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        TFT_print((char *)setup_labels[i], 8, y);
        char v[24]; setup_val(i, v, sizeof(v));
        TFT_print(v, _width - TFT_getStringWidth(v) - 10, y);
    }
    _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    TFT_print("VA/FM; TR1 gates the ADSR; turn to scroll", 8, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void sy_adj(int i, int dir)
{
    float d = (float)dir;
    switch (i) {
        case 0: sy.engine += dir; if (sy.engine < 0) sy.engine = ENG_WT; if (sy.engine > ENG_WT) sy.engine = ENG_VA; break;
        case 1: sy.base_note += dir; if (sy.base_note < 12) sy.base_note = 12; if (sy.base_note > 96) sy.base_note = 96; break;
        case 2: sy.quantize = !sy.quantize; break;
        case 3: sy.shape += d * 0.05f; if (sy.shape < 0) sy.shape = 0; if (sy.shape > 1) sy.shape = 1; break;
        case 4: sy.fm_ratio += d * 0.25f; if (sy.fm_ratio < 0.25f) sy.fm_ratio = 0.25f; if (sy.fm_ratio > 16) sy.fm_ratio = 16; break;
        case 5: sy.fm_index += d * 0.25f; if (sy.fm_index < 0) sy.fm_index = 0; if (sy.fm_index > 12) sy.fm_index = 12; break;
        case 6: sy.atk += d * 0.005f; if (sy.atk < 0.0005f) sy.atk = 0.0005f; if (sy.atk > 2) sy.atk = 2; break;
        case 7: sy.dec += d * 0.01f;  if (sy.dec < 0.001f) sy.dec = 0.001f; if (sy.dec > 2) sy.dec = 2; break;
        case 8: sy.sus += d * 0.05f;  if (sy.sus < 0) sy.sus = 0; if (sy.sus > 1) sy.sus = 1; break;
        case 9: sy.rel += d * 0.02f;  if (sy.rel < 0.001f) sy.rel = 0.001f; if (sy.rel > 3) sy.rel = 3; break;
        case 10: sy.env_to_cut += d * 0.05f; if (sy.env_to_cut < 0) sy.env_to_cut = 0; if (sy.env_to_cut > 1) sy.env_to_cut = 1; break;
        case 11: sy.glide += d * 0.02f; if (sy.glide < 0) sy.glide = 0; if (sy.glide > 2) sy.glide = 2; break;
        case 12: sy.lfo_rate += d * 0.25f; if (sy.lfo_rate < 0.05f) sy.lfo_rate = 0.05f; if (sy.lfo_rate > 20) sy.lfo_rate = 20; break;
        case 13: sy.lfo_depth += d * 0.05f; if (sy.lfo_depth < 0) sy.lfo_depth = 0; if (sy.lfo_depth > 1) sy.lfo_depth = 1; break;
        case 14: sy.lfo_dest += dir; if (sy.lfo_dest < 0) sy.lfo_dest = 2; if (sy.lfo_dest > 2) sy.lfo_dest = 0; break;
        case 15: sy.level += d * 0.05f; if (sy.level < 0) sy.level = 0; if (sy.level > 1) sy.level = 1; break;
        case 16: {   // reverb mode (lazy-init the tank on first non-off)
            int m = sy.rv.mode + dir;
            if (m < 0) m = RV_N_MODES - 1;
            if (m >= RV_N_MODES) m = RV_OFF;
            if (m != RV_OFF && !sy.rv.slab && reverb_init(&sy.rv) != ESP_OK) m = RV_OFF;
            reverb_set_mode(&sy.rv, m);
            break;
        }
        case 17: {
            float w = sy.rv.wet + d * 0.05f;
            if (w < 0) w = 0;
            if (w > 1) w = 1;
            reverb_set_mix(&sy.rv, w);
            break;
        }
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
            if (pos == SY_LOAD_ITEM) return M_SYNTH_LOAD;   // -> wave browser
            sel = !sel; setup_redraw(pos, sel);
            break;
        case EV_LONG_PRESS: return M_SYNTH_LIVE;
        default: break;
    }
    return 0;
}

// ---- Load Wave: the shared sample browser -> wavetable ----------------------
static int synth_load_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        sample_browser_enter(false, "Load Wave", sy.wave_name);   // alphabetical
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1) {                                    // picked
        const char *sel = sample_browser_selected();
        if (synth_load_wave(sel) == 0) sy.engine = ENG_WT;   // loading a wave selects WT
        return M_SYNTH_SETUP;
    }
    if (r == 2) return M_SYNTH_SETUP;                // cancelled
    return 0;
}

static void synth_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_SYNTH_LIVE);  menusys_item_set_default_cb(_ms, M_SYNTH_LIVE, synth_live_handler);
    menusys_new_item(_ms, M_SYNTH_SETUP); menusys_item_set_default_cb(_ms, M_SYNTH_SETUP, synth_setup_handler);
    menusys_new_item(_ms, M_SYNTH_LOAD);  menusys_item_set_default_cb(_ms, M_SYNTH_LOAD, synth_load_handler);
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
