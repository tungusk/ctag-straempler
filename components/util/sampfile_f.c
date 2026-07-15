// sampfile — raw-FatFS side. Same parsers as the VFS TU via sampfile_int.h;
// bare FatFS paths (no /sdcard prefix — the f_open house rule).
#include <string.h>
#include <strings.h>
#include "sampfile_f.h"
#include "sampfile_int.h"

static size_t fat_read_at(void *ctx, long off, void *buf, size_t n)
{
    FIL *f = (FIL *)ctx;
    if (f_lseek(f, (FSIZE_t)off) != FR_OK) return 0;
    UINT br = 0;
    if (f_read(f, buf, (UINT)n, &br) != FR_OK) return 0;
    return br;
}

int sampfile_probe_f(FIL *f, sampfile_t *sf)
{
    long fsize = (long)f_size(f);
    int r = sf_parse(sf, fsize, fat_read_at, f);
    f_lseek(f, r == 0 ? (FSIZE_t)sf->data_off : 0);
    return r;
}

size_t sampfile_read_f(FIL *f, const sampfile_t *sf, int16_t *dst, size_t n)
{
    UINT br = 0;
    uint32_t stride = sf_stride(sf);
    if (f_read(f, dst, (UINT)(n * stride), &br) != FR_OK) return 0;
    size_t got = br / stride;
    if (!sf_native(sf)) sf_convert(sf, dst, got);
    return got;
}

static const char *const SF_EXTS_F[] = { ".RAW", ".WAV", ".AIF", ".AIFF" };
// SLICES was historically missing here (the FatFS resolver couldn't find a bare
// id living in usr/SLICES); added alongside DRUMS. Bound derives from the array.
static const char *const SF_DIRS_F[] = { "usr", "usr/REC", "usr/LOOPS", "usr/SLICES", "usr/DRUMS" };
#define SF_N_DIRS_F ((int)(sizeof(SF_DIRS_F)/sizeof(SF_DIRS_F[0])))

int sample_resolve_f(const char *id, char *path, size_t path_len)
{
    FILINFO fi;
    for (int d = 0; d < SF_N_DIRS_F; d++)
        for (int i = 0; i < 4; i++) {
            snprintf(path, path_len, "%s/%s%s", SF_DIRS_F[d], id, SF_EXTS_F[i]);
            if (f_stat(path, &fi) == FR_OK) return 0;
        }
    snprintf(path, path_len, "usr/%s.RAW", id);
    return -1;
}
