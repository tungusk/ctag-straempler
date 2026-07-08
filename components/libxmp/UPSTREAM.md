# libxmp vendoring

- **Version**: 4.7.1 (upstream commit `12f76e4bf1dd6765e3bf46cbc1ff35c4a4551671`)
- **Source**: https://github.com/libxmp/libxmp
- **License**: MIT (see LICENSE)

## What's vendored

Full player + the complete non-depacker/non-prowizard loader set (~50 formats:
MOD/XM/IT/S3M/669/MTM/OKT/ULT/FAR/MED/DBM/AMF/PTM/… — everything in libxmp's
`format_loaders[]` except the ProWizard cracked-module converters).

## What's excluded

- `src/depackers/` — archive/compression depackers (gzip/xz/lha/…). Compiled
  out via `LIBXMP_NO_DEPACKERS`; source not copied. Modules must be
  uncompressed (they are, as `usr/*.MOD` etc.).
- `src/loaders/prowizard/` — ProWizard format converters. Compiled out via
  `LIBXMP_NO_PROWIZARD`; source not copied.
- `src/lite/` — libxmp-lite variant (unused).

## Local modifications

- **None to upstream .c/.h** (clean vendor — refresh by re-copying `src/`,
  `src/loaders/` top level + `include/xmp.h`).
- `CMakeLists.txt`, `xmp_mem.c`, `UPSTREAM.md`, `LICENSE` are ours.
- Heap redirected to PSRAM-first via `-Dmalloc=…` defines → `xmp_mem.c`.

## Flash cost (measured 2026-07-08, -Os, IDF 4.3)

~258 KB flash (223 KB code + 41 KB rodata), 1 KB static internal RAM.
