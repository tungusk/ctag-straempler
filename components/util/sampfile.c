// sampfile — VFS side: probe/read/resolve for the sample pool. Parsers are
// shared with the raw-FatFS twin (sampfile_f.c) via sampfile_int.h.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include "sd_lock.h"
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

int sampwav_start(FILE *f)
{
    // canonical 44-byte header; sizes are placeholders until sampwav_finish
    static const uint8_t hdr[44] = {
        'R','I','F','F', 0xFF,0xFF,0xFF,0xFF, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0,
        1,0,                       // PCM
        2,0,                       // stereo
        0x44,0xAC,0,0,             // 44100
        0x10,0xB1,2,0,             // byte rate 176400
        4,0,                       // block align
        16,0,                      // bits
        'd','a','t','a', 0xFF,0xFF,0xFF,0xFF,
    };
    return fwrite(hdr, 1, 44, f) == 44 ? 0 : -1;
}

int sampwav_finish(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long sz = ftell(f);
    if (sz < 44) return -1;
    uint32_t data = (uint32_t)(sz - 44);
    uint32_t riff = (uint32_t)(sz - 8);
    uint8_t b[4];
    b[0] = riff; b[1] = riff >> 8; b[2] = riff >> 16; b[3] = riff >> 24;
    fseek(f, 4, SEEK_SET);
    fwrite(b, 1, 4, f);
    b[0] = data; b[1] = data >> 8; b[2] = data >> 16; b[3] = data >> 24;
    fseek(f, 40, SEEK_SET);
    fwrite(b, 1, 4, f);
    return 0;
}

static const char *const SF_EXTS[] = { ".RAW", ".WAV", ".AIF", ".AIFF" };
#define SF_N_EXTS 4

static const char *const SF_DIRS[] = { "usr", "usr/REC", "usr/LOOPS", "usr/SLICES" };
#define SF_N_DIRS 4

int sample_resolve(const char *id, char *path, size_t path_len)
{
    struct stat st;
    for (int d = 0; d < SF_N_DIRS; d++)
        for (int i = 0; i < SF_N_EXTS; i++) {
            snprintf(path, path_len, "/sdcard/%s/%s%s", SF_DIRS[d], id, SF_EXTS[i]);
            if (stat(path, &st) == 0) return 0;
        }
    // default (callers get a well-formed path for error messages)
    snprintf(path, path_len, "/sdcard/usr/%s.RAW", id);
    return -1;
}

int sample_resolve_aux(const char *id, const char *ext, char *path, size_t path_len)
{
    char apath[96];
    if (sample_resolve(id, apath, sizeof(apath)) == 0) {
        char *dot = strrchr(apath, '.');
        if (dot) {
            *dot = 0;
            snprintf(path, path_len, "%s%s", apath, ext);
            return 0;
        }
    }
    snprintf(path, path_len, "/sdcard/usr/%s%s", id, ext);
    return -1;
}

int sample_next_index(const char *prefix)
{
    int plen = (int)strlen(prefix);
    int maxn = -1;
    for (int d = 0; d < SF_N_DIRS; d++) {
        char dp[32];
        snprintf(dp, sizeof(dp), "/sdcard/%s", SF_DIRS[d]);
        sd_lock_take();
        DIR *dir = opendir(dp);
        sd_lock_give();
        if (!dir) continue;
        for (;;) {
            // bus held per readdir step so audio refills interleave
            sd_lock_take();
            struct dirent *e = readdir(dir);
            sd_lock_give();
            if (e == NULL) break;
            if (strncasecmp(e->d_name, prefix, plen) != 0) continue;
            int n = atoi(e->d_name + plen);
            if (n > maxn) maxn = n;
        }
        sd_lock_take();
        closedir(dir);
        sd_lock_give();
    }
    return maxn + 1;
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
