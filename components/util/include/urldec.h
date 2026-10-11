// in-place URL query decoding (%XX and '+'), for handlers that read a value
// with httpd_query_key_value — it returns the raw, still-encoded text, and the
// page sends names through encodeURIComponent (ids like PNOF#3 carry '#')
#pragma once
#include <ctype.h>
#include <stdlib.h>

static inline void urldec_inplace(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else *o++ = *s++;
    }
    *o = 0;
}
