// Verbose boot screen for a card without bootlogo.bmp: the name in the big
// font, then boot-log lines in the fixed 8x12 font, left-aligned like a BIOS,
// and a quip at the bottom. The display fonts stop at ASCII, so the umlaut is
// two squares drawn over the A.
#include <string.h>
#include <stdio.h>
#include "tft.h"
#include "tftspi.h"
#include "bootscreen.h"

#define MARGIN 6
#define ROW    12                                   // SMALL_FONT is 8x12, fixed

void bootscreen_draw(const char *version, const char *const *lines, int n){
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK;
    font_transparent = 0;

    // the name
    TFT_setFont(DEJAVU24_FONT, NULL);
    _fg = TFT_CYAN;
    const char *name = "STRAMPLER";
    int fh = TFT_getfontheight();
    int x = MARGIN, y = 10;
    TFT_print((char *)name, x, y);
    int cx = x + TFT_getStringWidth("STR") + TFT_getStringWidth("A") / 2;   // over the A
    int d  = fh / 7;
    TFT_fillRect(cx - d - d / 2 - 1, y - d - 2, d, d, _fg);
    TFT_fillRect(cx + d / 2 + 1,     y - d - 2, d, d, _fg);

    // the log: version tag (the first word of version.txt), then the caller's lines
    TFT_setFont(SMALL_FONT, NULL);
    y += fh + 10;
    char tag[48];
    snprintf(tag, sizeof tag, "%s", version ? version : "");
    char *sp = strchr(tag, ' ');
    if (sp) *sp = 0;
    _fg = TFT_WHITE;
    TFT_print(tag, x, y); y += ROW;
    _fg = TFT_LIGHTGREY;
    for (int i = 0; i < n && y < _height - 3 * ROW; i++, y += ROW)
        TFT_print((char *)lines[i], x, y);

    // the quip, with the cursor a verbose boot leaves blinking
    _fg = TFT_GREEN;
    y = _height - 2 * ROW - MARGIN;
    TFT_print("wrover, wanderer, nomad, vagabond,", x, y); y += ROW;
    TFT_print("call me what you will", x, y);
    TFT_fillRect(TFT_X + 2, y + 2, 7, ROW - 3, _fg);
}
