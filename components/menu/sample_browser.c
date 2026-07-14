// The shared two-level sample browser. See sample_browser.h for the contract.
// The list level is the exact centered big-name renderer the six machines
// each carried a copy of; the folder level is new (organization, not
// restriction — "all" keeps the old flat browse one click away).
#include <stdio.h>
#include <string.h>
#include "tft.h"
#include "tftspi.h"
#include "sample_ram.h"
#include "ui_events.h"
#include "sample_browser.h"

#define BR_ROWS (SAMPLE_DIR_N + 1)   // "all" + one per folder

static struct {
    int  level;          // 0 = folder screen, 1 = list
    int  fsel;           // folder row 0..BR_ROWS-1 (0 = all)
    int  dir;            // SAMPLE_DIR_* chosen for the list level
    int  counts[SAMPLE_DIR_N];
    bool recent;
    char title[28];
    char current[24];
    char (*list)[24];
    int  n, sel;
} b;

static const char *const k_rows[BR_ROWS] = {"ALL", "POOL", "REC", "LOOPS", "SLICES"};

static void folder_draw(void)
{
    TFT_resetclipwin();
    _bg = TFT_BLACK; TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _fg = TFT_LIGHTGREY;
    char h[64];
    snprintf(h, sizeof(h), "%s - folder", b.title);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    int total = 0;
    for (int i = 0; i < SAMPLE_DIR_N; i++) total += b.counts[i];
    for (int i = 0; i < BR_ROWS; i++) {
        int y = fh + 22 + i * (fh + 14);
        _bg = (i == b.fsel) ? (color_t){10, 18, 56} : TFT_BLACK;
        _fg = (i == b.fsel) ? TFT_WHITE : (color_t){150, 150, 150};
        TFT_fillRect(0, y - 4, _width, fh + 10, _bg);
        TFT_print((char *)k_rows[i], 24, y);
        char c[16];
        snprintf(c, sizeof(c), "%d", i == 0 ? total : b.counts[i - 1]);
        TFT_print(c, _width - TFT_getStringWidth(c) - 24, y);
    }
    _bg = TFT_BLACK;
    _fg = (color_t){90, 90, 90}; TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:folder  press:open  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

static void list_refresh(void)
{
    b.n = b.recent ? sample_list_recent_dir(b.dir, &b.list)
                   : sample_list_shared_dir(b.dir, &b.list);
    b.sel = 0;
    if (b.current[0])
        for (int i = 0; i < b.n; i++)
            if (strcmp(b.list[i], b.current) == 0) { b.sel = i; break; }
}

static void list_draw(void)
{
    TFT_resetclipwin(); TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[64];
    snprintf(h, sizeof(h), "%s [%s]  (%d/%d)", b.title, sample_dir_name(b.dir),
             b.n ? b.sel + 1 : 0, b.n);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);
    if (!b.n) {
        char *m = "empty folder";
        TFT_print(m, _width / 2 - TFT_getStringWidth(m) / 2, _height / 2);
    } else {
        int cy = _height / 2;
        Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
        int bigfh = TFT_getfontheight(); _fg = TFT_WHITE;
        char *selnm = b.list[b.sel];
        TFT_print(selnm, _width / 2 - TFT_getStringWidth(selnm) / 2, cy - bigfh / 2);
        cfont = f;
        _fg = (color_t){110, 110, 110};
        for (int k = 1; k <= 4; k++) {
            int up = b.sel - k, dn = b.sel + k;
            int yup = cy - bigfh / 2 - k * (fh + 4) - 4;
            int ydn = cy + bigfh / 2 + (k - 1) * (fh + 4) + 6;
            if (up >= 0) { char *nm = b.list[up]; TFT_print(nm, _width / 2 - TFT_getStringWidth(nm) / 2, yup); }
            if (dn < b.n) { char *nm = b.list[dn]; TFT_print(nm, _width / 2 - TFT_getStringWidth(nm) / 2, ydn); }
        }
    }
    _fg = (color_t){90, 90, 90}; TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = "turn:browse  press:load  hold:folders";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

void sample_browser_enter(bool recent, const char *title, const char *current)
{
    b.level = 0;
    b.fsel = 0;
    b.recent = recent;
    snprintf(b.title, sizeof(b.title), "%s", title ? title : "Load Sample");
    snprintf(b.current, sizeof(b.current), "%s", current ? current : "");
    sample_folder_counts(b.counts);
    folder_draw();
}

const char *sample_browser_selected(void)
{
    return (b.n && b.sel >= 0 && b.sel < b.n) ? b.list[b.sel] : "";
}

int sample_browser_event(int event)
{
    if (b.level == 0) {
        switch (event) {
            case EV_FWD: b.fsel = (b.fsel + 1) % BR_ROWS; folder_draw(); break;
            case EV_BWD: b.fsel = (b.fsel + BR_ROWS - 1) % BR_ROWS; folder_draw(); break;
            case EV_SHORT_PRESS:
                b.dir = (b.fsel == 0) ? SAMPLE_DIR_ALL : b.fsel - 1;
                b.level = 1;
                list_refresh();
                list_draw();
                break;
            case EV_LONG_PRESS: return 2;   // cancel out of the browser
            default: break;
        }
        return 0;
    }
    switch (event) {
        case EV_FWD: if (b.n) { b.sel = (b.sel + 1) % b.n; list_draw(); } break;
        case EV_BWD: if (b.n) { b.sel = (b.sel + b.n - 1) % b.n; list_draw(); } break;
        case EV_SHORT_PRESS:
            if (b.n) return 1;              // machine loads selected()
            break;
        case EV_LONG_PRESS:                 // back OUT to the folder screen
            b.level = 0;
            sample_folder_counts(b.counts);
            folder_draw();
            break;
        default: break;
    }
    return 0;
}
