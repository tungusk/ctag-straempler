// sampfile — VFS side: probe/read/resolve for the sample pool. Parsers are
// shared with the raw-FatFS twin (sampfile_f.c) via sampfile_int.h.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "sampfile_int.h"

static size_t vfs_read_at(void *ctx, long off, void *buf, size_t n)
{
    FILE *f = (FILE *)ctx;
    if (fseek(f, off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, f);
}

int sampfile_probe(FILE *f, sampfile_t *sf)
{
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    int r = sf_parse(sf, fsize, vfs_read_at, f);
    fseek(f, r == 0 ? (long)sf->data_off : 0, SEEK_SET);
    return r;
}

size_t sampfile_read(FILE *f, const sampfile_t *sf, int16_t *dst, size_t n)
{
    if (sf_native(sf))                       // RAW / stereo-16 WAV: bytes ARE frames
        return fread(dst, 4, n, f);
    size_t got = fread(dst, sf_stride(sf), n, f);
    sf_convert(sf, dst, got);
    return got;
}

static const char *const SF_EXTS[] = { ".RAW", ".WAV", ".AIF", ".AIFF" };
#define SF_N_EXTS 4

int sample_resolve(const char *id, char *path, size_t path_len)
{
    struct stat st;
    for (int i = 0; i < SF_N_EXTS; i++) {
        snprintf(path, path_len, "/sdcard/usr/%s%s", id, SF_EXTS[i]);
        if (stat(path, &st) == 0) return 0;
    }
    // default (callers get a well-formed path for error messages)
    snprintf(path, path_len, "/sdcard/usr/%s.RAW", id);
    return -1;
}

bool sample_name_id(const char *fname, char *id_out, size_t id_len)
{
    int L = (int)strlen(fname);
    for (int i = 0; i < SF_N_EXTS; i++) {
        int el = (int)strlen(SF_EXTS[i]);
        if (L > el && strcasecmp(fname + L - el, SF_EXTS[i]) == 0) {
            int keep = L - el;
            if (keep >= (int)id_len) keep = (int)id_len - 1;
            memcpy(id_out, fname, keep);
            id_out[keep] = 0;
            return true;
        }
    }
    return false;
}
