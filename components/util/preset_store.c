// Named-preset store (see preset_store.h) — lifted verbatim from the Synth #23
// patch code and parameterized on {dir, pfx} so every machine gets save/name/
// recall from one implementation. Files are <dir>/<PFX>NNN.jsn, VFS paths.
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include "fileio.h"
#include "sd_lock.h"
#include "preset_store.h"

#define PS_EXT ".jsn"

static void ps_path(const preset_store_t *ps, const char *id, char *out, size_t n)
{
    snprintf(out, n, "%s/%s%s", ps->dir, id, PS_EXT);
}

// a preset file? -> strip the extension into out and return true
static bool ps_id(const preset_store_t *ps, const char *fname, char *out, size_t n)
{
    int L = (int)strlen(fname), el = (int)strlen(PS_EXT);
    if (L <= el || strcasecmp(fname + L - el, PS_EXT) != 0) return false;
    if (strncasecmp(fname, ps->pfx, strlen(ps->pfx)) != 0) return false;
    int keep = L - el;
    if (keep >= (int)n) keep = (int)n - 1;
    memcpy(out, fname, keep); out[keep] = 0;
    return true;
}

int preset_store_save(const preset_store_t *ps, cJSON *state, char *id_out, size_t n)
{
    char *txt = state ? cJSON_Print(state) : NULL;
    if (state) cJSON_Delete(state);
    if (!txt) return -1;

    struct stat st;
    int maxn = -1;
    sd_lock_take();
    if (stat(ps->dir, &st) != 0) mkdir(ps->dir, 0777);
    DIR *d = opendir(ps->dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char id[12];
            if (ps_id(ps, e->d_name, id, sizeof(id))) {
                int k = atoi(id + strlen(ps->pfx));
                if (k > maxn) maxn = k;
            }
        }
        closedir(d);
    }
    sd_lock_give();

    int idx = maxn + 1; if (idx < 0) idx = 0; if (idx > 999) idx = 999;
    char id[16]; snprintf(id, sizeof(id), "%s%03d", ps->pfx, idx);

    char path[64]; ps_path(ps, id, path, sizeof(path));
    sd_lock_take();
    writeJSONFile(path, txt);
    sd_lock_give();
    free(txt);
    if (id_out) snprintf(id_out, n, "%s", id);
    return 0;
}

cJSON *preset_store_load(const preset_store_t *ps, const char *id)
{
    if (!id || !id[0]) return NULL;
    char path[64]; ps_path(ps, id, path, sizeof(path));
    sd_lock_take();
    cJSON *root = readJSONFileAsCJSON(path);
    sd_lock_give();
    return root;
}

int preset_store_list(const preset_store_t *ps, char ids[][12], int max)
{
    int n = 0;
    sd_lock_take();
    DIR *d = opendir(ps->dir);
    if (d) {
        struct dirent *e;
        while (n < max && (e = readdir(d)) != NULL)
            if (ps_id(ps, e->d_name, ids[n], 12)) n++;
        closedir(d);
    }
    sd_lock_give();
    for (int i = 1; i < n; i++) {                     // insertion sort, descending
        char key[12]; snprintf(key, sizeof(key), "%s", ids[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(ids[j], key) < 0) { snprintf(ids[j+1], 12, "%s", ids[j]); j--; }
        snprintf(ids[j+1], 12, "%s", key);
    }
    return n;
}
