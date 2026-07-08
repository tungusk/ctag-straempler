// PSRAM-first allocator wrappers for the vendored libxmp component.
//
// libxmp calls plain malloc/calloc/realloc/strdup throughout. On this board
// plain malloc() lands in internal RAM only (CONFIG_SPIRAM_USE_MALLOC is off,
// SPIRAM is caps-alloc only) and internal RAM is scarce (~250 KB shared with
// WiFi + audio buffers + stacks). A loaded module (samples + patterns) is
// hundreds of KB to a couple MB, so it MUST live in PSRAM.
//
// The component CMakeLists redirects the allocating libc calls to these
// wrappers via -Dmalloc=xmp_mem_alloc etc. (that macro also rewrites the
// declarations pulled in from stdlib.h inside every libxmp TU, so these
// wrappers must carry exact libc signatures). We #undef the macros here so
// this file alone calls the real allocators.
//
// free() is deliberately NOT redirected: in ESP-IDF free() resolves the
// owning heap from the pointer and frees blocks from either heap, so libxmp
// freeing a PSRAM block through plain free() is correct. Likewise
// heap_caps_realloc handles cross-heap moves.

#undef malloc
#undef calloc
#undef realloc
#undef strdup

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

// PSRAM (external, byte-addressable) first; fall back to internal on failure
// so a tiny alloc during a PSRAM-pressure moment still succeeds.
#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CAPS_INT   (MALLOC_CAP_8BIT)

void *xmp_mem_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS_PSRAM);
    if (!p) p = heap_caps_malloc(size, CAPS_INT);
    return p;
}

void *xmp_mem_calloc(size_t nmemb, size_t size)
{
    void *p = heap_caps_calloc(nmemb, size, CAPS_PSRAM);
    if (!p) p = heap_caps_calloc(nmemb, size, CAPS_INT);
    return p;
}

void *xmp_mem_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, CAPS_PSRAM);
    if (!p && size) p = heap_caps_realloc(ptr, size, CAPS_INT);
    return p;
}

char *xmp_mem_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)xmp_mem_alloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
