// The dated browser walk. Lives in its OWN translation unit because raw FatFS
// (ff.h) and VFS (dirent.h, used by sample_ram.c) both typedef DIR — including
// the two in one file is a redefinition error.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "ff.h"          // raw FatFS: the timestamps ride along with the names
#include "sd_lock.h"
#include "sample_ram.h"
#include "sampfile.h"

int sample_list_recent_dir(int only, char (**out)[24])
{
    static char (*list)[24] = NULL;
    static uint32_t when[SAMPLE_LIST_RECENT_MAX];   // FatFS date<<16|time
    if (!list) {
        list = heap_caps_malloc(SAMPLE_LIST_RECENT_MAX * 24, MALLOC_CAP_SPIRAM);
        if (!list) { *out = NULL; return 0; }
    }
    int n = 0;
    sd_lock_take();
    // f_readdir hands over the timestamps in the SAME pass (a per-file stat()
    // would re-scan the directory once per entry). All pool folders feed the
    // one dated list — or just the one folder the browser is inside.
    static const char *const dirs[] = {"usr", "usr/REC", "usr/LOOPS", "usr/SLICES"};
    for (int di = 0; di < SAMPLE_DIR_N; di++) {
    if (only >= 0 && di != only) continue;
    FF_DIR d;
    if (f_opendir(&d, dirs[di]) == FR_OK) {         // FatFS path: no /sdcard
        FILINFO fi;
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            char id[24];
            if (!sample_name_id(fi.fname, id, sizeof(id))) continue;
            // one id per base across containers: the resolver (.RAW first)
            // decides which file a loader gets
            bool dup = false;
            for (int k = 0; k < n; k++)
                if (strcasecmp(list[k], id) == 0) { dup = true; break; }
            if (dup) continue;
            uint32_t w = ((uint32_t)fi.fdate << 16) | fi.ftime;
            int slot = n;
            if (n >= SAMPLE_LIST_RECENT_MAX) {
                // full: evict the OLDEST if this file is newer. A first-N-seen
                // cap silently hid every fresh take once the library outgrew it
                // ("can't find recordings above 140") — the drums-era lesson.
                int oldest = 0;
                for (int k = 1; k < SAMPLE_LIST_RECENT_MAX; k++)
                    if (when[k] < when[oldest]) oldest = k;
                if (w <= when[oldest]) continue;
                slot = oldest;
            } else {
                n++;
            }
            strcpy(list[slot], id);   // extension-less, dedup'd above
            when[slot] = w;
        }
        f_closedir(&d);
    }
    }
    sd_lock_give();
    // insertion sort, NEWEST FIRST — fresh takes land at the top of the browser
    // (Arlo); name breaks ties so equal-dated files keep a stable order
    for (int i = 1; i < n; i++) {
        char tmp[24];
        uint32_t tw = when[i];
        memcpy(tmp, list[i], 24);
        int j = i - 1;
        while (j >= 0 && (when[j] < tw ||
                          (when[j] == tw && strcasecmp(list[j], tmp) > 0))) {
            memcpy(list[j + 1], list[j], 24);
            when[j + 1] = when[j];
            j--;
        }
        memcpy(list[j + 1], tmp, 24);
        when[j + 1] = tw;
    }
    *out = list;
    return n;
}

int sample_list_recent(char (**out)[24])
{
    return sample_list_recent_dir(SAMPLE_DIR_ALL, out);
}
