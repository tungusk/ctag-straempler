// Text-only boot screen for a card without bootlogo.bmp — the name in the big
// font and the version tag under it. The display fonts stop at ASCII, so the
// umlaut is two squares drawn over the A.
#include <string.h>
#include <stdio.h>
#include "tft.h"
#include "tftspi.h"
#include "bootscreen.h"

void bootscreen_draw(const char *version){
    TFT_fillScreen(TFT_BLACK);
    _bg = TFT_BLACK;
    _fg = TFT_CYAN;
    font_transparent = 0;
    TFT_setFont(DEJAVU24_FONT, NULL);
    const char *name = "STRAMPLER";
    int fh = TFT_getfontheight();
    int w  = TFT_getStringWidth((char *)name);
    int x  = (_width - w) / 2;
    int y  = _height / 2 - fh;                         // name just above the middle
    TFT_print((char *)name, x, y);
    // the umlaut: the A is the fourth glyph; two squares centred over it
    int xa = x + TFT_getStringWidth("STR");
    int cx = xa + TFT_getStringWidth("A") / 2;
    int d  = fh / 7;                                   // square side, ~3 px at 24
    int ya = y - d - 2;
    TFT_fillRect(cx - d - d / 2 - 1, ya, d, d, _fg);
    TFT_fillRect(cx + d / 2 + 1,     ya, d, d, _fg);
    // the version tag: the first word of version.txt, the rest is prose
    char tag[48];
    snprintf(tag, sizeof tag, "%s", version ? version : "");
    char *sp = strchr(tag, ' ');
    if (sp) *sp = 0;
    TFT_setFont(DEFAULT_FONT, NULL);
    _fg = TFT_WHITE;
    TFT_print(tag, CENTER, y + fh + 10);
}
