#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freesound.h"
#include "fs_auth.h"

bool fs_auth_ok(void)
{
    const char *t = freesoundGetToken();
    return t != NULL && t[0] != 0;
}

int fs_auth_query_suffix(char *buf, size_t len)
{
    if (!fs_auth_ok()) { if (len) buf[0] = 0; return -1; }
    snprintf(buf, len, "&token=%s", freesoundGetToken());
    return 0;
}
