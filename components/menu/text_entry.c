// Shared encoder text entry (see text_entry.h). Lifted verbatim from
// settings_input_def_handler in menu.c so the WiFi/Api-Key page keeps its exact
// feel; the only changes are that the buffer and cursor live here instead of in
// the handler's statics, and that accept/cancel are return codes instead of
// menu ids.
#include <string.h>
#include <ctype.h>
#include "tft.h"
#include "tftspi.h"
#include "menu_types.h"
#include "menutft.h"
#include "ui_events.h"
#include "text_entry.h"

// The character wheel. Order matters: digits first so a numeric string is a
// short turn from the top, then upper, then lower, then the punctuation a
// password needs, and the three sentinels last.
static const char C_LIST[] =
    "=0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_ !?<^";
#define C_MAX 69                       // last index in C_LIST

static struct {
    char buf[TEXT_ENTRY_MAX + 2];      // +NUL, +1 slack for the write-then-clamp below
    int  pos;                          // cursor = next write position
    int  c;                            // index into C_LIST
    int  maxlen;
} te;

static void draw_cursor(print_ids_t id)
{
    menuTFTPrintChar(te.buf, te.pos, C_LIST[te.c], id);
}

void text_entry_enter(const char *title, const char *initial, int maxlen)
{
    memset(&te, 0, sizeof(te));
    te.maxlen = (maxlen > 0 && maxlen <= TEXT_ENTRY_MAX) ? maxlen : TEXT_ENTRY_MAX;
    // 42 ('f') is where the settings page has always opened: it set c = 41 and
    // then FELL THROUGH into its EV_FWD case, which stepped once before drawing.
    // Kept deliberately — an "obvious fix" to 1 ('0') would change the feel of
    // the page this was factored out of.
    te.c = 42;

    menuTFTResetTextWrap();
    menuTFTPrintInputMenu((char *)title);

    if (initial && initial[0]) {
        strlcpy(te.buf, initial, sizeof(te.buf));
        te.pos = menuTFTPrintAllCharSettings(te.buf);
        if (te.pos > te.maxlen) te.pos = te.maxlen;
    }
    draw_cursor(PRINT_NORM);
}

int text_entry_event(int event)
{
    switch (event) {
        case EV_FWD:
            if (te.c < C_MAX) te.c++;
            draw_cursor(PRINT_NORM);
            break;

        case EV_BWD:
            if (te.c > 0) te.c--;
            draw_cursor(PRINT_NORM);
            break;

        case EV_SHORT_PRESS:
            switch (C_LIST[te.c]) {
                case '^':
                    return 2;
                case '<':
                    // terminate where we are, then step back ONTO the previous
                    // character — it stays in the buffer until it is overwritten
                    te.buf[te.pos] = '\0';
                    if (te.pos > 0) te.pos--;
                    draw_cursor(PRINT_NORM);
                    break;
                case '=':
                    if (te.pos == 0) break;          // refuse an empty accept
                    te.buf[te.pos] = '\0';
                    return 1;
                default:
                    te.buf[te.pos] = C_LIST[te.c];
                    if (te.pos < te.maxlen) te.pos++;
                    draw_cursor(PRINT_NORM);
                    break;
            }
            break;

        case EV_LONG_PRESS:
            // Commit the current character uppercased. '^' and '<' ignore the
            // hold, but '=' deliberately does NOT: a long press is the only way
            // to type a literal '=' (short press there means accept), and a
            // WiFi password may well contain one.
            if (C_LIST[te.c] == '^' || C_LIST[te.c] == '<') break;
            te.buf[te.pos] = (char)toupper((unsigned char)C_LIST[te.c]);
            draw_cursor(PRINT_UPPER);
            if (te.pos < te.maxlen) te.pos++;
            draw_cursor(PRINT_NORM);
            break;

        default:
            break;
    }
    return 0;
}

const char *text_entry_result(void)
{
    te.buf[TEXT_ENTRY_MAX + 1] = '\0';
    return te.buf;
}
