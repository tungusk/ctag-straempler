#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freesound.h"
#include "fs_auth.h"

bool fs_auth_ok(void)
{
    const char *t = freesoundGetToken();
    // A card that has never been configured carries the literal placeholder
    // fileio.c writes into a fresh CONFIG.JSN. Treating that as a real key made
    // the panel report "key ok" and then every search came back 401 — the worst
    // kind of wrong, because it points the blame at the network.
    return t != NULL && t[0] != 0 && strcmp(t, "myapikey") != 0;
}

int fs_auth_query_suffix(char *buf, size_t len)
{
    if (!fs_auth_ok()) { if (len) buf[0] = 0; return -1; }
    snprintf(buf, len, "&token=%s", freesoundGetToken());
    return 0;
}
