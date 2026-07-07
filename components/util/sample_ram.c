#include "sample_ram.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "esp_log.h"
#include "sd_lock.h"

int sample_list(char out[][24], int max)
{
    sd_lock_take();   // brief name-only walk; hold the bus for its duration
    DIR *d = opendir("/sdcard/usr");
    if (!d) { sd_lock_give(); return 0; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        int L = strlen(e->d_name);
        if (L > 4 && strcasecmp(e->d_name + L - 4, ".RAW") == 0) {
            int idl = L - 4;
            if (idl > 23) idl = 23;
            memcpy(out[n], e->d_name, idl);
            out[n][idl] = 0;
            n++;
        }
    }
    closedir(d);
    sd_lock_give();
    return n;
}

static char s_shared_list[SAMPLE_LIST_MAX][24];

static int cmp_name24(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

int sample_list_shared(char (**out)[24])
{
    int n = sample_list(s_shared_list, SAMPLE_LIST_MAX);
    qsort(s_shared_list, n, sizeof(s_shared_list[0]), cmp_name24);
    *out = s_shared_list;
    return n;
}

uint32_t sample_load(const char *name, int16_t *dst, uint32_t max_frames, bool mono)
{
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", name);
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    sd_lock_give();
    if (!f) { ESP_LOGE("SAMPLE", "cannot open %s", path); return 0; }

    uint32_t n = 0;
    int32_t rbuf[256];
    size_t got;
    for (;;) {
        if (n >= max_frames) break;
        // one 1 KB read per lock hold, released between so audio/REST interleave
        sd_lock_take();
        got = fread(rbuf, sizeof(int32_t), 256, f);
        sd_lock_give();
        if (got == 0) break;
        for (size_t k = 0; k < got && n < max_frames; k++) {
            int16_t l = (int16_t)(rbuf[k] & 0xFFFF);
            int16_t r = (int16_t)(rbuf[k] >> 16);
            if (mono) dst[n] = (int16_t)(((int)l + r) / 2);
            else { dst[n * 2] = l; dst[n * 2 + 1] = r; }
            n++;
        }
    }
    sd_lock_take();
    fclose(f);
    sd_lock_give();
    return n;
}
