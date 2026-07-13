#pragma once
// sampfile — raw-FatFS twin (see sampfile.h for the contract). Separate
// header because ff.h and dirent.h both typedef DIR; only FatFS-world TUs
// (deck_analysis, recording) may include this one.
#include "ff.h"
#include "sampfile.h"

int    sampfile_probe_f(FIL *f, sampfile_t *sf);
size_t sampfile_read_f(FIL *f, const sampfile_t *sf, int16_t *dst, size_t n);
// id -> bare FatFS pool path ("usr/<id>.<ext>", no /sdcard prefix)
int    sample_resolve_f(const char *id, char *path, size_t path_len);
