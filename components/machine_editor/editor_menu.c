// Editor panel UI. Until 2026-09-12 this was a status word and the sentence
// "pick a sample + op in the web Editor tab" — the machine could not be driven
// from the rack at all.
//
// Now the live page is the shared waveform strip (components/menu/wave_edit.c,
// lifted from Tape) over the Editor's streaming file backend, so the material
// can be minutes long and stereo. Setup carries the ops. Everything the web
// card could do, the panel can do; the web now mirrors it instead of owning it.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "menusys.h"
#include "menu_types.h"
#include "menutft.h"
#include "setup_menu.h"
#include "sample_browser.h"
#include "wave_edit.h"
#include "ui_events.h"
#include "tft.h"
#include "tftspi.h"
#include <esp_http_server.h>
#include "machine.h"
#include "sample_ram.h"
#include "editor_priv.h"

static const color_t DIM    = {110, 110, 120};
static const color_t OKCOL  = {40, 200, 90};
static const color_t ERRCOL = {220, 60, 40};
static const color_t WORK   = {230, 170, 0};

static bool s_skip_clear = false;
#define CLEAR_RECT(x, y, w, h) do { if (!s_skip_clear) TFT_fillRect((x), (y), (w), (h), TFT_BLACK); } while (0)

static wave_edit_t s_wave;
static int s_setup_return = -1;

// ---- geometry ---------------------------------------------------------------
static int wy(void) { return TFT_getfontheight() * 2 + 16; }
static int wh(void)
{
    int h = _height - wy() - TFT_getfontheight() * 2 - 14;
    return h < 24 ? 24 : h;
}

static void secs(uint32_t fr, char *b, size_t n)
{
    float t = (float)fr / (float)ED_RATE;
    if (t >= 60.0f) snprintf(b, n, "%d:%05.2f", (int)(t / 60), t - 60.0f * (int)(t / 60));
    else            snprintf(b, n, "%.2fs", t);
}

static void wave_sync(void)
{
    s_wave.peaks   = ed.peaks;
    s_wave.n_peaks = ED_PEAKS;
    s_wave.frames  = ed.frames;
    s_wave.in_pt   = ed.in_pt;
    s_wave.out_pt  = ed.out_pt;
    s_wave.x = 2;
    s_wave.y = wy();
    s_wave.w = _width - 4;
    s_wave.h = wh();
    s_wave.play = editor_auditioning() ? editor_play_pos() : WE_NO_PLAY;
}

// ---- header / readout -------------------------------------------------------
static unsigned s_hdr_sig = 0, s_rd_sig = 0;

static unsigned hdr_sig(void)
{
    unsigned h = (unsigned)ed.state * 7919u + (unsigned)ed.progress * 131u + (unsigned)ed.scan_pct;
    for (const char *p = ed.src; *p; p++) h = h * 31u + (unsigned char)*p;
    for (const char *p = ed.out; *p; p++) h = h * 31u + (unsigned char)*p;
    return h ? h : 1;
}
static unsigned rd_sig(void)
{
    return ed.in_pt * 2654435761u + ed.out_pt * 40503u
         + (unsigned)s_wave.cursor * 7u + (s_wave.grabbed ? 3u : 0u)
         + (unsigned)s_wave.view_len;
}

static void header(void)
{
    int fh = TFT_getfontheight();
    CLEAR_RECT(0, 0, _width, fh * 2 + 14);
    _bg = TFT_BLACK; _fg = TFT_WHITE;
    TFT_print("Editor", 6, 4);

    char s[48];
    if (ed.src[0]) {
        _fg = TFT_LIGHTGREY;
        snprintf(s, sizeof(s), "%s", ed.src);
        TFT_print(s, 70, 4);
        secs(ed.frames, s, sizeof(s));
        _fg = DIM;
        TFT_print(s, _width - 6 - TFT_getStringWidth(s), 4);
    } else {
        _fg = DIM;
        TFT_print("no sample - hold for Setup > Source", 70, 4);
    }

    // second line: whatever is happening to the file right now
    char t[56];
    if (ed.scanning)              { _fg = WORK;   snprintf(t, sizeof(t), "scanning %d%%", ed.scan_pct); }
    else if (ed.state == ED_RUNNING) { _fg = WORK;   snprintf(t, sizeof(t), "%s %d%%", ed_op_names[ed.op], ed.progress); }
    else if (ed.state == ED_ERR)  { _fg = ERRCOL; snprintf(t, sizeof(t), "%s", ed.err); }
    else if (ed.state == ED_DONE) { _fg = OKCOL;  snprintf(t, sizeof(t), "wrote %s", ed.out); }
    else                          { _fg = DIM;    snprintf(t, sizeof(t), "%s", ed.clip_full ? "clipboard ready" : "non-destructive: every op writes a new take"); }
    while (t[0] && TFT_getStringWidth(t) > _width - 12) t[strlen(t) - 1] = 0;
    TFT_print(t, 6, fh + 8);
    s_hdr_sig = hdr_sig();
}

static void readout(void)
{
    static const char *const CN[WE_CURSOR_N] = { "IN", "OUT", "WIN", "ZOOM" };
    int fh = TFT_getfontheight();
    int y = _height - fh * 2 - 8;
    CLEAR_RECT(0, y - 2, _width, fh + 6);
    _bg = TFT_BLACK;
    char a[24], b[24], s[64];
    secs(ed.in_pt, a, sizeof(a));
    secs(ed.out_pt, b, sizeof(b));
    _fg = TFT_LIGHTGREY;
    snprintf(s, sizeof(s), "IN %s   OUT %s", a, b);
    TFT_print(s, 6, y);
    _fg = s_wave.grabbed ? (color_t){30, 215, 90} : TFT_CYAN;
    // the play marker gives TR1 visible feedback without costing a redraw
    snprintf(s, sizeof(s), "%s%s%s", editor_auditioning() ? "> " : "",
             CN[s_wave.cursor], s_wave.grabbed ? "*" : "");
    TFT_print(s, _width - 6 - TFT_getStringWidth(s), y);
    s_rd_sig = rd_sig();
}

static void hint(void)
{
    _bg = TFT_BLACK; _fg = (color_t){90, 90, 90};
    TFT_setFont(DEF_SMALL_FONT, NULL);
    CLEAR_RECT(0, _height - TFT_getfontheight() - 2, _width, TFT_getfontheight() + 2);
    TFT_print("turn:pick  press:grab  hold:setup  TR1:play/stop", 6, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void live_full_redraw(void)
{
    TFT_resetclipwin();
    TFT_fillScreen(TFT_BLACK);
    s_skip_clear = true;
    wave_sync();
    header();
    wave_edit_draw(&s_wave);
    readout();
    hint();
    s_skip_clear = false;
}

static int editor_live_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    switch (event) {
        case EV_ENTERED_MENU:
            live_full_redraw();
            break;

        case EV_TIMER_REPEATING_FAST:
            // TR1 edges land here, not in the audio task that saw them
            if (editor_trig_consume()) {
                editor_audition(!editor_auditioning());
                readout();
            }
            // the playhead is the only thing allowed to move at 300 ms — a full
            // strip repaint on this tick would starve the PSRAM audio path
            if (editor_auditioning()) { wave_sync(); wave_edit_playhead(&s_wave); }
            if (hdr_sig() != s_hdr_sig) header();
            break;

        case EV_TIMER_REPEATING_SLOW:
            if (hdr_sig() != s_hdr_sig) header();
            // a scan finishing is the one thing that changes the waveform itself
            if (!ed.scanning && s_wave.frames != ed.frames) { wave_sync(); wave_edit_draw(&s_wave); }
            break;

        case EV_FWD: case EV_BWD: case EV_SHORT_PRESS: case EV_LONG_PRESS: {
            wave_sync();
            int r = wave_edit_event(&s_wave, event);
            if (r == WE_EXIT) return M_EDITOR_SETUP;
            if (r == WE_MOVED) {
                ed.in_pt  = s_wave.in_pt;
                ed.out_pt = s_wave.out_pt;
                editor_audition_window();       // the loop follows the crop
            }
            if (r != WE_NONE) readout();
            break;
        }
        default: break;
    }
    return 0;
}

// ============================================================================
// Setup
// ============================================================================
enum { ER_SOURCE = 0, ER_OP, ER_PARAM, ER_APPLY, ER_AUDITION,
       ER_COPY, ER_CUT, ER_PASTE, ER_SLICES, ER_SLICE, ER_ZOOMALL, ER_N };

static const setup_item_t ed_setup_items[] = {
    {"Source",    ST_ACTION},
    {"Op",        ST_TOGGLE},
    {"Param",     ST_RANGE},
    {"Apply",     ST_ACTION},
    {"Audition",  ST_TOGGLE},
    {"Copy",      ST_ACTION},
    {"Cut",       ST_ACTION},
    {"Paste",     ST_ACTION},
    {"Slices",    ST_RANGE},
    {"Slice",     ST_ACTION},
    {"Zoom All",  ST_ACTION},
};
_Static_assert(sizeof(ed_setup_items) / sizeof(ed_setup_items[0]) == ER_N,
               "ed_setup_items and ER_* disagree — a stale count drops the tail rows");

static int s_slices = 8;

// guard the ops that need a loaded source, and SAY so in the value column
// rather than silently doing nothing (the house pattern — no confirm dialogs)
static const char *need_src(void) { return ed.src[0] ? NULL : "load first"; }

static void ed_setup_val(int i, char *v, size_t n)
{
    const char *g = need_src();
    switch (i) {
        case ER_SOURCE:   snprintf(v, n, "%s", ed.src[0] ? ed.src : "-"); break;
        case ER_OP:       snprintf(v, n, "%s", ed_op_names[ed.op]); break;
        case ER_PARAM:
            if (ed.op == OP_FADEIN || ed.op == OP_FADEOUT) snprintf(v, n, "%.0f ms", ed.param > 0 ? ed.param : 50.0f);
            else if (ed.op == OP_TRIM)                     snprintf(v, n, "thr %d", ed.param > 0 ? (int)ed.param : 150);
            else                                           snprintf(v, n, "-");
            break;
        case ER_APPLY:    snprintf(v, n, "%s", g ? g : (ed.state == ED_RUNNING ? "working" : "go >")); break;
        case ER_AUDITION: snprintf(v, n, "%s", editor_auditioning() ? "on" : "off"); break;
        case ER_COPY:     snprintf(v, n, "%s", g ? g : "copy >"); break;
        case ER_CUT:      snprintf(v, n, "%s", g ? g : "cut >"); break;
        case ER_PASTE:    snprintf(v, n, "%s", ed.clip_full ? "paste >" : "empty"); break;
        case ER_SLICES:   snprintf(v, n, "%d", s_slices); break;
        case ER_SLICE:    snprintf(v, n, "%s", g ? g : "slice >"); break;
        case ER_ZOOMALL:  snprintf(v, n, "fit >"); break;
        default:          v[0] = 0; break;
    }
}

static void ed_setup_adj(int i, int dir)
{
    switch (i) {
        case ER_OP: {
            int o = ed.op + dir;
            if (o < 0) o = OP_N - 1;
            if (o >= OP_N) o = 0;
            ed.op = o;
            break;
        }
        case ER_PARAM:
            if (ed.op == OP_FADEIN || ed.op == OP_FADEOUT) {
                float f = (ed.param > 0 ? ed.param : 50.0f) + dir * 10.0f;
                ed.param = f < 1.0f ? 1.0f : (f > 5000.0f ? 5000.0f : f);
            } else if (ed.op == OP_TRIM) {
                float t = (ed.param > 0 ? ed.param : 150.0f) + dir * 10.0f;
                ed.param = t < 1.0f ? 1.0f : (t > 8000.0f ? 8000.0f : t);
            }
            break;
        case ER_AUDITION:
            editor_audition(!editor_auditioning());
            break;
        case ER_SLICES:
            s_slices += dir;
            if (s_slices < 1) s_slices = 1;
            if (s_slices > 64) s_slices = 64;
            break;
        default: break;
    }
}

static int ed_setup_action(int i)
{
    switch (i) {
        case ER_SOURCE:  s_setup_return = ER_SOURCE; return M_EDITOR_LOAD;
        case ER_APPLY:   editor_apply(ed.src, ed.op, ed.param, ed.in_pt, ed.out_pt); break;
        case ER_COPY:    editor_copy();  break;
        case ER_CUT:     editor_cut();   break;
        case ER_PASTE:   editor_paste(); break;
        case ER_SLICE:   editor_slice(s_slices); break;
        case ER_ZOOMALL: wave_edit_zoom_all(&s_wave); return M_EDITOR_LIVE;
        default: break;
    }
    return 0;
}

static setup_menu_t ed_setup = {
    .items = ed_setup_items,
    .n = (int)(sizeof(ed_setup_items) / sizeof(ed_setup_items[0])),
    .title = "Editor Setup",
    .aff_label = "Machine",
    .aff_target = M_MORE,
    .live_target = M_EDITOR_LIVE,
    .render = ed_setup_val,
    .adjust = ed_setup_adj,
    .action = ed_setup_action,
};

static int editor_setup_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU && s_setup_return >= 0) {
        int p = s_setup_return;
        s_setup_return = -1;
        setup_menu_enter_at(&ed_setup, p);      // NOTE: this DRAWS
        return 0;
    }
    return setup_menu_event(&ed_setup, event);
}

// ---- browser page -----------------------------------------------------------
static int editor_load_handler(int it_id, int event, void *ev_data)
{
    (void)it_id; (void)ev_data;
    if (event == EV_ENTERED_MENU) {
        sample_browser_enter(true, "Edit Sample", ed.src);
        return 0;
    }
    int r = sample_browser_event(event);
    if (r == 1) {
        editor_audition(false);                 // the old file is about to close
        editor_load(sample_browser_selected());
        wave_sync();
        wave_edit_zoom_all(&s_wave);
        s_setup_return = ER_SOURCE;
        return M_EDITOR_SETUP;
    }
    if (r == 2) { s_setup_return = ER_SOURCE; return M_EDITOR_SETUP; }
    return 0;
}

// ---- registration -----------------------------------------------------------
static void editor_register_pages(void *menusys)
{
    menusys_t *_ms = (menusys_t *)menusys;
    menusys_new_item(_ms, M_EDITOR_LIVE);  menusys_item_set_default_cb(_ms, M_EDITOR_LIVE,  editor_live_handler);
    menusys_new_item(_ms, M_EDITOR_SETUP); menusys_item_set_default_cb(_ms, M_EDITOR_SETUP, editor_setup_handler);
    menusys_new_item(_ms, M_EDITOR_LOAD);  menusys_item_set_default_cb(_ms, M_EDITOR_LOAD,  editor_load_handler);
}

static int editor_main_event(int event, void *ev_data)
{
    (void)ev_data;
    if (event == EV_ENTERED_MENU || event == EV_TIMER_REPEATING_SLOW) {
        int fh = TFT_getfontheight();
        _bg = TFT_BLACK;
        TFT_fillRect(0, fh + 12, _width, 24, _bg);
        char s[56];
        if (ed.state == ED_RUNNING) { _fg = WORK; snprintf(s, sizeof(s), "Editor: %s %d%%", ed_op_names[ed.op], ed.progress); }
        else if (ed.src[0])         { _fg = TFT_LIGHTGREY; snprintf(s, sizeof(s), "Editor: %s", ed.src); }
        else                        { _fg = DIM; snprintf(s, sizeof(s), "Editor: pick a sample in Setup"); }
        TFT_print(s, 6, fh + 16);
    }
    return 0;
}

extern const httpd_uri_t editor_web_uris[];

static const char *const editor_main_items[] = { "Live", "Setup" };
static const int editor_main_targets[] = { M_EDITOR_LIVE, M_EDITOR_SETUP };

const machine_ui_t editor_menu_ui = {
    .main_items = editor_main_items,
    .main_targets = editor_main_targets,
    .n_main = 2,
    .register_pages = editor_register_pages,
    .main_event = editor_main_event,
    .boot_target = M_EDITOR_LIVE,
    .web_uris = editor_web_uris,
    .n_web_uris = 2,
    .setup = &ed_setup,
};
