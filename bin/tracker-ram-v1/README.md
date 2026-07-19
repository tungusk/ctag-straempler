# tracker-ram-v1 (2026-07-18)

Flashable snapshot. OTA-capable layout (ota_0/ota_1 + otadata). WiFi update:
`tools/ota.sh <IP>`. Serial recovery: `bin/tracker-ram-v1/flash.sh [PORT]`.

## What's in this milestone (on top of tapeui-v1)
The deep **Tracker internal-RAM fix**. Tracker "worked flawlessly when new" but
had started failing on switch-in with **"no RAM for render"** (and sometimes
crashing on a heavy `.IT`). libxmp's render task wants a **32 KB INTERNAL** stack
and, by the time you switched in, the largest contiguous internal block was
often below that.

### Root cause
NOT the shine encoder / reverb / FX / radio buffers — those are all **PSRAM**
(verified), zero internal cost. The culprit was **task stacks** (always internal
DRAM): when the shine/MP3 broadcast landed, `audio_broadcast_init()` began
spawning a **12 KB internal `bc_srv` task stack at every boot**, plus a 4 KB
internal send buffer held forever — ~16 KB gone from power-on whether or not
anyone streamed. (`tracker.c` already blamed it: *"bc_srv grew for shine"*.)

### Fix
- **Broadcast is lazy and OFF by default.** `audio_broadcast_set_enabled(bool)` /
  `audio_broadcast_enabled()` replace `audio_broadcast_init()`; boot reads the
  new `settings.broadcast` (default 0). The listener task self-frees its 12 KB
  stack + send buffer on disable (its `accept()` gained a 500 ms timeout so it
  notices the disable and tears down). Off → that 12 KB is free when Tracker
  switches in.
- **Send buffer moved to PSRAM** (was an internal `malloc`); the ~176 KB stream
  ring is now lazily allocated in PSRAM and shared with the icecast push.
- **Three off-switches:** System → Settings → **Broadcast** (toggle row,
  persisted), a web **Enable/Disable** button on the BROADCAST card, and
  `GET /bcast/enable?on=0|1`. `/bcast/state` now also reports `enabled`.
- The trimmed-stack **safety cap** (refuse an oversized module instead of
  panicking) was already present in `tracker.c` — this milestone removes the
  *cause*, so the cap is now just the belt.
- **Bounce** was checked and left as-is: its recorder allocates on start and
  frees on stop, so it holds no idle internal RAM.

### Verified (OTA-flashed, on hardware)
Broadcast off at boot; Tracker switches in and renders with no "no RAM for
render"; the **heaviest module, NSFG.IT (982 KB)**, loads and plays with no
crash-reboot; enabling/disabling broadcast mid-playback doesn't crash; the
`:8000/` WAV stream still serves correctly (from the PSRAM buffer) when enabled.

## NOT yet
- Disabling broadcast while a listener is mid-stream stops new accepts but frees
  the 12 KB only after that client disconnects (irrelevant to the boot-off
  default). A sub-500 ms OFF→ON toggle can no-op while the task tears down.
- `/remote/machine` still switches the audio machine but leaves the on-screen
  menu on the old machine (display-vs-audio desync, minor).
