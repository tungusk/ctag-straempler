// The shared sample browser — ONE flat list per open. The folder rows sit at
// the TOP (each shown with a trailing '/': ALL/ POOL/ REC/ LOOPS/ SLICES/),
// the current folder's files below them. Turn to scroll the whole thing; press
// a folder row to switch which files show, press a file to load it, hold to
// cancel. The browse position (folder + selection) is REMEMBERED across opens
// (Arlo). Replaces the old two-screen folder-picker-then-list widget.
//
// Folders are organization, not restriction: "ALL" keeps the flat browse, and
// per-folder lists turn the 224/512 name caps into per-folder budgets.
#include <stdio.h>
#include <string.h>
#include "tft.h"
#include "tftspi.h"
#include "sample_ram.h"
#include "ui_events.h"
#include "sample_browser.h"

#define BR_NFOLD (SAMPLE_DIR_N + 1)   // "ALL" + one row per real folder

// static so the position survives across opens ("remembers where it was")
static struct {
    int  dir;            // current folder: SAMPLE_DIR_ALL or 0..SAMPLE_DIR_N-1
    int  sel;            // index into the COMBINED list [folders.. then files]
    int  counts[SAMPLE_DIR_N];
    bool recent;
    bool primed;         // seeded a sensible default on the very first open
    char title[28];
    char (*list)[24];
    int  n;              // files in the current folder
} b;

static const char *const k_fold[BR_NFOLD] = {"ALL", "POOL", "REC", "LOOPS", "SLICES"};
static const color_t FOLD_FG  = {120, 205, 130};   // folder rows read green-ish
static const color_t FOLD_DIM = {70, 120, 75};

static int  fold_to_dir(int f){ return f == 0 ? SAMPLE_DIR_ALL : f - 1; }
static int  br_total(void){ return BR_NFOLD + b.n; }
static bool is_fold(int i){ return i < BR_NFOLD; }

static void list_refresh(void){
    b.n = b.recent ? sample_list_recent_dir(b.dir, &b.list)
                   : sample_list_shared_dir(b.dir, &b.list);
}

// display name for a combined index; folder rows get a trailing '/'
static const char *entry_name(int i, char *buf, size_t bn){
    if (is_fold(i)){ snprintf(buf, bn, "%s/", k_fold[i]); return buf; }
    int fi = i - BR_NFOLD;
    return (fi >= 0 && fi < b.n) ? b.list[fi] : "";
}

static void draw(void){
    int tot = br_total();
    if (b.sel < 0) b.sel = 0;
    if (b.sel >= tot) b.sel = tot - 1;
    TFT_resetclipwin(); TFT_fillScreen(TFT_BLACK);
    int fh = TFT_getfontheight();
    _bg = TFT_BLACK; _fg = TFT_LIGHTGREY;
    char h[64];
    int fcount = (b.dir == SAMPLE_DIR_ALL) ? b.n : b.counts[b.dir];
    snprintf(h, sizeof(h), "%s  [%s %d]", b.title, sample_dir_name(b.dir), fcount);
    TFT_print(h, _width / 2 - TFT_getStringWidth(h) / 2, 4);

    int cy = _height / 2;
    char buf[28];
    // the selection, big and centered
    Font f = cfont; TFT_setFont(DEJAVU24_FONT, NULL);
    int bigfh = TFT_getfontheight();
    _fg = is_fold(b.sel) ? FOLD_FG : TFT_WHITE;
    const char *snm = entry_name(b.sel, buf, sizeof(buf));
    TFT_print((char *)snm, _width / 2 - TFT_getStringWidth((char *)snm) / 2, cy - bigfh / 2);
    cfont = f;
    // four neighbors above and below, dimmed (folders keep their green tint)
    for (int k = 1; k <= 4; k++){
        int up = b.sel - k, dn = b.sel + k;
        int yup = cy - bigfh / 2 - k * (fh + 4) - 4;
        int ydn = cy + bigfh / 2 + (k - 1) * (fh + 4) + 6;
        char nb[28];
        if (up >= 0){
            const char *nm = entry_name(up, nb, sizeof(nb));
            _fg = is_fold(up) ? FOLD_DIM : (color_t){110, 110, 110};
            TFT_print((char *)nm, _width / 2 - TFT_getStringWidth((char *)nm) / 2, yup);
        }
        if (dn < tot){
            const char *nm = entry_name(dn, nb, sizeof(nb));
            _fg = is_fold(dn) ? FOLD_DIM : (color_t){110, 110, 110};
            TFT_print((char *)nm, _width / 2 - TFT_getStringWidth((char *)nm) / 2, ydn);
        }
    }
    _fg = (color_t){90, 90, 90}; TFT_setFont(DEF_SMALL_FONT, NULL);
    char *hint = is_fold(b.sel) ? "turn:browse  press:open  hold:cancel"
                                : "turn:browse  press:load  hold:cancel";
    TFT_print(hint, _width / 2 - TFT_getStringWidth(hint) / 2, _height - TFT_getfontheight() - 1);
    TFT_setFont(DEFAULT_FONT, NULL);
}

void sample_browser_enter(bool recent, const char *title, const char *current)
{
    (void)current;                       // position is remembered, not seeded
    b.recent = recent;
    snprintf(b.title, sizeof(b.title), "%s", title ? title : "Load");
    sample_folder_counts(b.counts);
    if (!b.primed){ b.dir = SAMPLE_DIR_POOL; b.sel = BR_NFOLD; b.primed = true; }
    list_refresh();
    draw();
}

const char *sample_browser_selected(void)
{
    int fi = b.sel - BR_NFOLD;
    return (fi >= 0 && fi < b.n) ? b.list[fi] : "";
}

int sample_browser_event(int event)
{
    int tot = br_total();
    switch (event){
        case EV_FWD: b.sel = (b.sel + 1) % tot; draw(); break;
        case EV_BWD: b.sel = (b.sel + tot - 1) % tot; draw(); break;
        case EV_SHORT_PRESS:
            if (is_fold(b.sel)){             // a folder row: switch which files show
                b.dir = fold_to_dir(b.sel);
                list_refresh();
                if (b.n) b.sel = BR_NFOLD;   // drop onto the folder's first file
                draw();
                break;
            }
            return 1;                        // a file: the machine loads selected()
        case EV_LONG_PRESS:
            return 2;                        // cancel out of the browser
        default: break;
    }
    return 0;
}
