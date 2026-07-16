// Shared Setup-menu framework — see setup_menu.h. Owns the scrollable list
// render + the press/turn grammar so every machine's Setup behaves identically.
#include "setup_menu.h"
#include "tft.h"
#include "tftspi.h"
#include "menutft.h"
#include "ui_events.h"
#include <stdio.h>

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

int setup_menu_event(setup_menu_t *m, int event)
{
    switch (event) {
        case EV_ENTERED_MENU:
            m->pos = 0; m->sel = 0;
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
