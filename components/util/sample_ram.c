#include "sample_ram.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "esp_log.h"

int sample_list(char out[][24], int max)
{
    DIR *d = opendir("/sdcard/usr");
    if (!d) return 0;
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
    return n;
}

uint32_t sample_load(const char *name, int16_t *dst, uint32_t max_frames, bool mono)
{
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/usr/%s.RAW", name);
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE("SAMPLE", "cannot open %s", path); return 0; }

    uint32_t n = 0;
    int32_t rbuf[256];
    size_t got;
    while (n < max_frames && (got = fread(rbuf, sizeof(int32_t), 256, f)) > 0) {
        for (size_t k = 0; k < got && n < max_frames; k++) {
            int16_t l = (int16_t)(rbuf[k] & 0xFFFF);
            int16_t r = (int16_t)(rbuf[k] >> 16);
            if (mono) dst[n] = (int16_t)(((int)l + r) / 2);
            else { dst[n * 2] = l; dst[n * 2 + 1] = r; }
            n++;
        }
    }
    fclose(f);
    return n;
}
