# libxmp 4.7.1 (upstream 12f76e4) vendored as an ESP-IDF component.
# Playback-only tracker-module library (MIT, see LICENSE). Depackers and
# ProWizard loaders are excluded (source not vendored + the LIBXMP_NO_*
# defines compile out their references). Full loader set is kept — the whole
# point is "all the formats".
file(GLOB LIBXMP_SRCS "src/*.c" "src/loaders/*.c")

idf_component_register(
    SRCS ${LIBXMP_SRCS} "xmp_mem.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "src/loaders")

# Redirect libxmp's heap use to PSRAM-first wrappers (xmp_mem.c). The macro
# also rewrites the stdlib.h declarations inside each libxmp TU, so the
# wrappers carry exact libc signatures; xmp_mem.c #undefs them for itself.
# strdup is unused in libxmp so it is not redirected. free() stays real —
# ESP-IDF free() frees blocks from either heap.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LIBXMP_NO_DEPACKERS
    LIBXMP_NO_PROWIZARD
    HAVE_POWF=1
    malloc=xmp_mem_alloc
    calloc=xmp_mem_calloc
    realloc=xmp_mem_realloc)

# -Os: this is size-sensitive (2 MB app partition). Warnings suppressed rather
# than patching upstream — libxmp is C90-portable but noisy under IDF's -Wall.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Os
    -Wno-error
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-type-limits)
