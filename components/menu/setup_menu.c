// Shared Setup-menu framework — see setup_menu.h. Owns the scrollable list
// render + the press/turn grammar so every machine's Setup behaves identically.
#include "setup_menu.h"
#include "tft.h"
#include "tftspi.h"
#include "menutft.h"
#include "ui_events.h"
#include <stdio.h>
#include <string.h>
#include "machine.h"      // machine_active() -> its Setup page for the web mirror

static void draw(setup_menu_t *m)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    TFT_setFont(DEFAULT_FONT, NULL);
    int fh = TFT_getfontheight();
    _fg = TFT_WHITE; TFT_print((char *)m->title, 6, 4);
    menuTFTPrintAffordance((char *)m->aff_label, m->pos == -1);

    // scrollable list — keep the cursor in view when items exceed the screen
    int row_h = fh + 5, y0 = fh + 14;
    int vis = (_height - fh - 6 - y0) / row_h;
    if (vis < 1) vis = 1;
    int top = 0;
    if (m->pos >= vis) top = m->pos - vis + 1;

    for (int r = 0; r < vis; r++) {
        int i = top + r;
        if (i >= m->n) break;
        int y = y0 + r * row_h;
        _bg = (i == m->pos) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == m->pos && m->sel) ? TFT_CYAN : TFT_WHITE;
        TFT_fillRect(0, y - 2, _width, fh + 4, _bg);
        TFT_print((char *)m->items[i].label, 8, y);

        char v[28]; v[0] = 0;
        if (m->render) m->render(i, v, sizeof(v));
        // value fixed at a right position padded by ONE char, so it never shifts
        // when selected; in RANGE edit mode brackets hug it directly (no spaces)
        bool editing = (i == m->pos && m->sel && m->items[i].kind == ST_RANGE);
        int cw = TFT_getStringWidth("]");
        int vw = TFT_getStringWidth(v);
        int vx = _width - 10 - cw - vw;
        TFT_print(v, vx, y);
        if (editing) {
            TFT_print("[", vx - cw, y);
            TFT_print("]", vx + vw, y);
        }
    }
}

// ---- web mirror (Remote tab, 2026-09-09): the active machine's Setup page as
// JSON, and a remote adjust that lands on the UI task like a knob turn. The
// page is NOT drawn from here — the caller re-enters the current page; if that
// is this Setup page the cursor lands on the edited row (s_rem_*), so the TFT
// follows the web edit instead of jumping to the top.
static setup_menu_t *s_rem_m = NULL;
static int s_rem_pos = -1;

static const setup_menu_t *active_setup(void)
{
    const machine_t *m = machine_active();
    return (m && m->ui) ? (const setup_menu_t *)m->ui->setup : NULL;
}
static int jesc(char *o, size_t n, const char *in)   // minimal JSON string escape
{
    size_t p = 0;
    for (; *in && p + 2 < n; in++) {
        if (*in == '"' || *in == '\\') o[p++] = '\\';
        if ((unsigned char)*in < 0x20) continue;
        o[p++] = *in;
    }
    o[p] = 0;
    return (int)p;
}
int setup_menu_remote_json(char *out, size_t n)
{
    const setup_menu_t *m = active_setup();
    if (!m) return snprintf(out, n, "{\"rows\":[]}");
    char t[40], a[40], l[40], v[40], raw[28];
    jesc(t, sizeof(t), m->title ? m->title : "");
    jesc(a, sizeof(a), m->aff_label ? m->aff_label : "");
    int p = snprintf(out, n, "{\"title\":\"%s\",\"aff\":\"%s\",\"pos\":%d,\"rows\":[", t, a, m->pos);
    for (int i = 0; i < m->n && p < (int)n - 96; i++) {
        raw[0] = 0;
        if (m->render) m->render(i, raw, sizeof(raw));
        jesc(l, sizeof(l), m->items[i].label ? m->items[i].label : "");
        jesc(v, sizeof(v), raw);
        p += snprintf(out + p, n - p, "%s{\"l\":\"%s\",\"k\":%d,\"v\":\"%s\"}",
                      i ? "," : "", l, (int)m->items[i].kind, v);
    }
    p += snprintf(out + p, n - p, "]}");
    return p;
}
// UI task only (menu.c, EV_REMOTE_SETUP). ACTION rows open sub-pages on the
// device and are not driven from the web.
void setup_menu_remote_adjust(int i, int dir, int cnt)
{
    setup_menu_t *m = (setup_menu_t *)active_setup();
    if (!m || i < 0 || i >= m->n || m->items[i].kind == ST_ACTION || !m->adjust) return;
    if (cnt < 1) cnt = 1;
    if (cnt > 50) cnt = 50;
    for (int k = 0; k < cnt; k++) m->adjust(i, dir < 0 ? -1 : +1);
    s_rem_m = m; s_rem_pos = i;
}

int setup_menu_event(setup_menu_t *m, int event)
{
    switch (event) {
        case EV_ENTERED_MENU:
            m->pos = (m == s_rem_m && s_rem_pos >= 0 && s_rem_pos < m->n) ? s_rem_pos : 0;
            s_rem_m = NULL; s_rem_pos = -1;
            m->sel = 0;
            draw(m);
            break;
        case EV_FWD:
            if (m->sel) { if (m->adjust) m->adjust(m->pos, +1); }     // editing a RANGE
            else { m->pos++; if (m->pos >= m->n) m->pos = -1; }        // navigate (wrap to affordance)
            draw(m);
            break;
        case EV_BWD:
            if (m->sel) { if (m->adjust) m->adjust(m->pos, -1); }
            else { m->pos--; if (m->pos < -1) m->pos = m->n - 1; }
            draw(m);
            break;
        case EV_SHORT_PRESS:
            if (m->pos == -1) return m->aff_target;                    // top affordance
            switch (m->items[m->pos].kind) {
                case ST_TOGGLE:                                        // press CYCLES
                    if (m->adjust) m->adjust(m->pos, +1);
                    draw(m);
                    break;
                case ST_RANGE:                                        // press enters/exits [ ] edit
                    m->sel = !m->sel;
                    draw(m);
                    break;
                case ST_ACTION: {                                     // press fires -> sub-page
                    int t = m->action ? m->action(m->pos) : 0;
                    if (t) return t;
                    draw(m);
                    break;
                }
            }
            break;
        case EV_LONG_PRESS:
            return m->live_target;
        default:
            break;
    }
    return 0;
}

// Enter a setup page with the cursor already on row `pos` instead of the top —
// e.g. returning from a sub-page to the line that opened it. Clamps out-of-range.
void setup_menu_enter_at(setup_menu_t *m, int pos)
{
    m->pos = (pos >= 0 && pos < m->n) ? pos : 0;
    m->sel = 0;
    draw(m);
}
