#include "sample_ram.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "esp_log.h"
#include "sd_lock.h"
#include "sampfile.h"

// every pool folder (flat usr/ = legacy + uploads, REC = takes, LOOPS =
// saves) can feed ONE flat id list, or be walked alone (the folder browser);
// loaders resolve ids back to folders either way
static const char *const dirs[] = {"/sdcard/usr", "/sdcard/usr/REC",
                                   "/sdcard/usr/LOOPS", "/sdcard/usr/SLICES",
                                   "/sdcard/usr/DRUMS", "/sdcard/usr/KEYS",
                                   "/sdcard/usr/TAPE"};
// indexed BY SAMPLE_DIR_* — a short array is an out-of-bounds read (see the
// sample_list_recent.c crash), so the count is a build-time invariant
_Static_assert(sizeof(dirs)/sizeof(dirs[0]) == SAMPLE_DIR_N, "one dir per SAMPLE_DIR_*");

const char *sample_dir_name(int di)
{
    static const char *const names[] = {"pool", "REC", "LOOPS", "SLICES", "DRUMS", "KEYS", "TAPE"};
    return (di >= 0 && di < SAMPLE_DIR_N) ? names[di] : "all";
}

void sample_folder_counts(int out[SAMPLE_DIR_N])
{
    // display counts for the folder screen: one name-only pass per folder,
    // NO cross-folder dedup (a base living in two folders counts in both —
    // that is what the browser will show when you enter each one)
    char id[24];
    for (int di = 0; di < SAMPLE_DIR_N; di++) {
        out[di] = 0;
        sd_lock_take();
        DIR *d = opendir(dirs[di]);
        if (!d) { sd_lock_give(); continue; }
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (sample_name_id(e->d_name, id, sizeof(id))) out[di]++;
        closedir(d);
        sd_lock_give();
    }
}

static int sample_list_dir(int only, char out[][24], int max)
{
    int n = 0;
    char id[24];
    for (int di = 0; di < SAMPLE_DIR_N && n < max; di++) {
        if (only >= 0 && di != only) continue;
        sd_lock_take();   // brief name-only walk; hold the bus for its duration
        DIR *d = opendir(dirs[di]);
        if (!d) { sd_lock_give(); continue; }
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max) {
            if (!sample_name_id(e->d_name, id, sizeof(id))) continue;
            // one id per base across containers AND folders (dedup; the
            // resolver — pool + RAW first — picks the file)
            bool dup = false;
            for (int k = 0; k < n; k++)
                if (strcasecmp(out[k], id) == 0) { dup = true; break; }
            if (dup) continue;
            strcpy(out[n], id);
            n++;
        }
        closedir(d);
        sd_lock_give();
    }
    return n;
}

int sample_list(char out[][24], int max)
{
    return sample_list_dir(SAMPLE_DIR_ALL, out, max);
}

static char s_shared_list[SAMPLE_LIST_MAX][24];

static int cmp_name24(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

int sample_list_shared_dir(int di, char (**out)[24])
{
    int n = sample_list_dir(di, s_shared_list, SAMPLE_LIST_MAX);
    qsort(s_shared_list, n, sizeof(s_shared_list[0]), cmp_name24);
    *out = s_shared_list;
    return n;
}

int sample_list_shared(char (**out)[24])
{
    return sample_list_shared_dir(SAMPLE_DIR_ALL, out);
}

uint32_t sample_load(const char *name, int16_t *dst, uint32_t max_frames, bool mono)
{
    char path[64];
    sample_resolve(name, path, sizeof(path));   // .RAW / .WAV / .AIF(F)
    sd_lock_take();
    FILE *f = fopen(path, "rb");
    sd_lock_give();
    if (!f) { ESP_LOGE("SAMPLE", "cannot open %s", path); return 0; }
    sampfile_t sf;
    sd_lock_take();
    int pr = sampfile_probe(f, &sf);
    sd_lock_give();
    if (pr != 0) {
        ESP_LOGE("SAMPLE", "%s: %s", path, sf.why);
        sd_lock_take(); fclose(f); sd_lock_give();
        return 0;
    }

    uint32_t n = 0;
    int16_t rbuf[256 * 2];                      // 256 native stereo frames
    size_t got;
    for (;;) {
        if (n >= max_frames) break;
        uint32_t want = max_frames - n;
        if (want > 256) want = 256;
        // one ~1 KB read per lock hold, released between so audio/REST interleave
        sd_lock_take();
        got = sampfile_read(f, &sf, rbuf, want);
        sd_lock_give();
        if (got == 0) break;
        for (size_t k = 0; k < got && n < max_frames; k++) {
            int16_t l = rbuf[k * 2], r = rbuf[k * 2 + 1];
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
