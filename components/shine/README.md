# shine — vendored fixed-point MP3 encoder

**Origin:** https://github.com/toots/shine, tag **3.1.1** (`src/lib/` only).
**License:** LGPL-2 (see `COPYING`) — compatible with publishing this fork's
source; do not strip the license file.

Shine is an integer-only MPEG-1 Layer III encoder (born on slow ARM devices),
which is why it can run on the ESP32 next to a machine's DSP. Used by the
output-broadcast server (`components/audio/audio.c`) to serve the live output
bus as MP3 on port 8000.

## Local patches (keep this list current)

1. `layer3.c` `shine_initialise()`: `calloc` → `heap_caps_calloc(...,
   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` — the ~100 KB encoder state (the
   40 KB `int2idx` LUT dominates) would exhaust the ~250 KB internal heap.
2. `CMakeLists.txt` (ours): compiled `-O2` (project default is `-Os`).

No other source changes; upgrades = re-copy `src/lib/` + re-apply the list.
