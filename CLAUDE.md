# ctag-straempler — build & hardware notes

## Build

**IDF version: 4.3 only.** IDF 4.4 causes audio crackling on both voices — do not upgrade.

Flash command must include `--flash_size detect`:
```
idf.py flash --flash_size detect
```
The sdkconfig defaults to 16MB but the hardware chip is 8MB. Omitting `detect` causes a crash loop on boot.

ULP build requires both toolchain bins in PATH before invoking `idf.py`. If only the xtensa bin is present, ULP cmake falls back to `/usr/bin/nm` and `mapgen.py` crashes:
```
export PATH=/path/to/xtensa-esp32-elf/bin:/path/to/esp32ulp-elf/bin:$PATH
```

## Hardware

**CV correction**: upstream docs claimed channels 3/4 (idx 2/3) are inverting op-amps, but THIS UNIT reads them straight (verified 2026-07-05; `cv_bipolar[]` in audio.h is now all-false). Still use the `cv_corrected(src, data)` helper for all modulation CV reads — it's the single place per-channel correction lives if hardware quirks resurface. Upstream's 5/6 ADC swap in spi_per.c was also wrong for this unit and has been removed.

**Trig inputs** — TRIG0=GPIO39, TRIG1=GPIO36 are the only digital gate inputs. They are ACTIVE LOW: idle reads high (1), a gate pulls low (0). A machine sees them as `machine_io_t.trig_level` bits (bit0=TRIG0/"TR1", bit1=TRIG1/"TR2"); detect the falling edge (1→0), and seed the previous state to idle-high (0x03) or the first block fires a phantom edge. The looper uses TR1 as its clock input by default.

**CV knob/jack channels (this unit's real map, verified 2026-07-05):** ch1/2 = 1V/oct jacks, idle ~880 (~21%) by analog design — floor-trim them if used as generic mod sources (sampler2's `s2_cv_floor`). ch3/4 = bipolar (±5V) inputs, idle mid-scale ~2048. **ch4 jack is broken** (reads pinned high) — don't map anything to it; Arlo will bench-fix. ch5-8 = knob+jack (knob attenuates a patched CV); **knobs 5 and 8 cap incoming CV at ~half level** (5's normalization also weak) — so knobs 6 and 7 are the two fully-good ones. The looper deliberately maps CV6/CV7 to the selected track's level/pan for this reason.

**No mic input in Eurorack** — the `codec_set_input()` function exists but the hardware is always wired for line in. Don't add mic-mode logic.

**No CV/gate OUTPUT** — the only output is audio via the I2S codec (WM8731 line out on GPIO22). There is no CV/gate DAC anywhere; a machine's `process()` can only write the two audio channels, and those outputs are AC-coupled (can't hold a DC level). So true analog CV/gates are NOT achievable — any "output" feature must be an audio-rate signal. (Input side is rich: 8 CV in + 2 gate in.)

**Audio block size:** 64 samples per I2S DMA block at 44100 Hz (~1.45ms per audio loop tick).

## Code rules

**FatFS paths only** — file paths passed to `f_open()` must NOT have a `/sdcard` prefix. VFS-style paths (`/sdcard/...`) cause an `f_open` abort crash. Use bare paths like `usr/REC_0001.RAW`.

**SD bus is serialized by `sd_lock`** (`components/util/sd_lock.{h,c}`) — a global recursive mutex that EVERY SD I/O burst must hold: the audio raw-FatFS reads, REST file serving, recording writer, config/JSON, `sample_ram`. Acquired INSIDE the per-voice `file_mutex` (consistent order → no deadlock), released between bursts. The raw audio read path bypasses the esp_vfs_fat lock, so without this it races VFS I/O and wedges/corrupts the card. Don't add SD access that skips `sd_lock`.

**SDMMC DMA can't target PSRAM** — read SD data into INTERNAL DMA-capable RAM (`heap_caps_malloc(sz, MALLOC_CAP_DMA)`), never a PSRAM buffer, or `sdmmc_read_blocks` fails `ESP_ERR_NO_MEM (257)` under memory pressure (its internal bounce-buffer alloc fails). This bit `readJSONFileAsCJSON` (empty web browser) — reads into PSRAM failed for every sidecar. Internal RAM is tight (~250 KB total); prefer STREAMING responses (see `/files`) over building big buffers, which also OOMs.

**No core pinning for tasks that read files** — pinning reader tasks to core 0 causes WiFi preemption and produces constant clicks on both voices. Leave file-reading tasks unpinned.

**Sleep at least one tick.** `CONFIG_FREERTOS_HZ=100`: one tick is 10 ms, so
`pdMS_TO_TICKS(n)` for n<10 is ZERO and `vTaskDelay(0)` never yields to
LOWER-priority tasks — an "idle" loop built on it is a busy-spin that starves
httpd/readers depending on scheduler luck (the intermittent "REST dead while
audio plays" family, and "arming mutes both tracks"). Use `vTaskDelay(1)`
minimum in every task loop; grep single-digit `pdMS_TO_TICKS` in new code.

**`audio_status_t` access** — always use `audio_get_status()` (portMUX spinlock protected). Never read `_audio_status` directly from outside `audio.c`.

**Display corruption = memory corruption.** A years-old `sprintf("Single Shot")` into `char tmp[10]` (a 2-byte stack smash on every playmode redraw) masqueraded as an intermittent, firmware-dependent "white screen / stale-pixel" hardware fault for a long time — the blast radius shifted with each build's stack layout. Lesson: size string buffers generously and when the display corrupts, suspect a smash before the panel/ribbon/bus. `TFT_setclipwin` state and the `_fg/_bg/cfont` globals are shared — save/restore them around draws.

## Machine architecture

The core hosts swappable "machines" (independent sampling/processing modes),
one active at a time, switched at runtime via **System → Machine** (choice
persisted as `"machine"` in CONFIG.JSN). Plan + full history:
`/Users/arlo/.claude/plans/machine-architecture.md`.

- **Interface**: `components/machine/include/machine.h` — a `machine_t` has
  `start/stop/process/preset_save/preset_load` + a `machine_ui_t`. `process()`
  runs in the audio task once per 64-sample block; no SD/heap/blocking there.
- **Registry**: `main/machine_registry.c` is the ONLY file outside a machine's
  own component that may name a machine symbol. Registry (selector order):
  Sampler2 / Sampler / Looper / Slicer / Granular / Glitch / Drums / Deck /
  Tracker / Freesound / Stub. (Display names: "Sampler" = the deck-pattern
  rebuild in machine_sampler3; "Sampler2" = the legacy `s2_` fork in
  machine_sampler2, kept as a fallback until sampler3 has a full hardware
  verdict, then scheduled for removal. The frozen original machine_sampler
  ["Sampler0"] was deleted 2026-07-12 — upstream v0.9 remains available via
  git history and bin/ archives.) **Stub AND Sampler2 are HIDDEN from the
  System→Machine selector** (skipped by name in both selector paths in
  `menu.c`; Sampler2 stays reachable via the web Remote tab's machine
  switch) but stay in the registry as fallback + proof target; the selector
  uses a parallel `machines[]` array so hidden entries don't desync the
  on-screen index.
- **Machine web URIs**: a machine may publish REST endpoints served only while
  it is active (`machine_ui_t.web_uris` = `const httpd_uri_t[]`); the core
  registers/unregisters them on switch via `machine_set_web_cb()`
  (machine_core ↔ rest-api, boot-order independent).
- **Core owns**: boot, SD + sample library, TFT + menusys shell, encoder/UI
  events, WiFi + REST, CV acquisition, I2S transport, recording service.
  **Machine owns**: everything between input and output.
- **Per-machine autosave**: `AUTOSAVE.JSN` keys each machine's `preset_save`
  state under its name, so machines remember settings independently across
  switches/reboots. `"machine"` in CONFIG.JSN persists the boot choice.
- **Adding a machine**: new `components/machine_<name>/` (see machine_glitch as
  the smallest clean example — engine + menu + priv header + CMakeLists), add
  one line to the registry + `main/CMakeLists.txt` REQUIRES, add it to
  `tools/proof_build.sh`'s EXCLUDE list, add `M_<NAME>_*` menu IDs to
  `components/menu/include/menu_types.h`, and bump the selector cap in `menu.c`
  (`names[16]`/`machines[16]`/`n<16`) if the roster exceeds it.
- **Proof invariant**: `tools/proof_build.sh` must pass — the firmware links
  with every real machine excluded and a stub-only registry. Run it after any
  change touching core or the registry.

The machines (all working; archives in `bin/`):
- `machine_sampler3` ("Sampler") — two-voice clock-time LOOP RECORDER
  rebuilt on the deck architecture (2026-07-12): one unpinned reader task
  owns all SD I/O; per-voice 1s PSRAM head-cache (instant gate retrig) +
  4s ring in playback-order frame space (reader maps reverse). CROP
  windows are sampler2-style START+LENGTH, engine-side cursor math
  (CV-rate performable, no head rebuild): modes OFF / FREE / QUANT / QUANTx2
  (points snap to whole beats of the take's bpm stamp). CV MATRIX:
  per-voice Speed/Start/Length, each assigned any of CV1..8 (ch1/2
  floor-trimmed; all reads median-of-5 conditioned — WiFi ADC spikes).
  WINDOWED STREAMING: while looping, the stream is capped past the window
  so the ring keeps it — wraps are seamless cursor math with a ~6ms seam
  crossfade; full-file wraps prefetch the rewind under the playing head;
  a starved-cursor self-heal (+150ms sledgehammer) re-fires the seek
  protocol, wedge-proof. No pitch/v-oct by design (that's the future
  instrument-sampler machine). Internal clock (Record page, 40-240 bpm)
  drives the whole synced-record workflow without an external clock.
  Explicit Record page + ARM/REC banner (Live encoder turn disarms),
  auto-pickup of finished recordings, version-gated autosave (s3v:2).
  JOINT GRID SNAP: hold BOTH gates ~1s and both loops restart from their
  window starts on the same clock pulse. Live UI: mirrored side-by-side
  panels (track 2 right-justified), playbar = black canvas + bold white
  waveform with the FAT state-colored box's ends sitting AT the crop
  points (the box IS the loop window), total length under the bar.
- `machine_sampler2` ("Sampler2", HIDDEN) — legacy `s2_`-prefixed fork:
  crop mode, signed CV matrix amounts, CV-addressable start/length.
  Patched 2026-07-12 (deferred auto-load, DMA-capable SD buffers, no
  abort-on-missing-file) but retains at least one residual race (WDT
  panic-in-panic minutes into record sessions) — fallback only, removal
  pending sampler3's full hardware verdict.
- `machine_looper` — 4-track clock-synced RAM looper, save-to-library, per-track
  BP filter. House-style UI (2026-07-13): waveform-thumbnail lanes w/ state-colored
  playhead + slice redraws, click-toggle Setup rows w/ [ value ] bracket edits
- `machine_slicer` — one stereo sample, grid OR transient slicing + a
  sensitivity dial-in screen
- `machine_granular` — 16-grain cloud over a mono sample (raised-cosine grains)
- `machine_glitch` — live-input stutter/beat-repeat (no SD), clock beat-sync
- `machine_drumsampler` ("Drums") — FOUR one-shot mono RAM pads (the 8-pad mode
  is gone: a pad can carry two sounds now, which is what 8 was for). Each pad
  may hold a second CHOKE LAYER (per-pad `layered`): a B sample on its own CV
  trigger sharing ONE voice, so either hit interrupts the other (open/closed
  hi-hat) — 4 cells, 8 sounds, 8 triggers. Per layer: buffer, trigger, Schmitt
  state; per pad: everything performable. Buffers are LAZY (an empty kit costs
  no PSRAM; a failed alloc fails soft). Trigger modes unchanged (Direct
  floor-tracking Schmitt per CV; CV-select = TRIG1/2 + selector CV, and with
  layers TR1 fires A, TR2 fires B). PERFORMANCE: knob6/knob7 drive whatever the
  encoder is pointing at, both NEUTRAL at noon, taking over only when moved —
  knob6 = level (noon = unity, CW drives up to 4x through a cubic soft clipper),
  knob7 = CCW decay choke, CW one of four per-pad targets (retrig loop / attack
  / start / none). Retrig has a repeat cap: sample-length, a count, or INF.
  MASTER FILTER: a box in the middle of the menu bar that the encoder selects
  like a pad (knob6 = the deck's DJ sweep, knob7 = resonance, which forces a
  lower stability ceiling + a damping floor that rises with cutoff + a NaN
  guard). Live grid: a layered pad draws as two half-cells (own dot, name,
  trigger tag, rectified half-wave converging on the midline); the selection box
  is the HALF; the encoder traces the grid circularly
  (1A > filter > 2A > 2B > 4A > 4B > 3B > 3A > 1B).
- `machine_deck` ("Deck") — tempo-syncing track player: streams long
  usr/*.RAW from SD through a PSRAM ring (reader task; process() never
  touches SD), varispeed playback phase-locked to the external CV clock
  (pulse-level PLL, so clock mult/div — 1/4..4 pulses per beat — works),
  offline BPM + beat-grid detection (onset flux autocorrelation) cached as
  "bpm"/"grid" in the track's JSN sidecar, AUTO-run on loading an
  unanalyzed track (coexists with playback; snapshots its track name so a
  mid-analysis load can't poison the new track's sidecar). TR1 = restart
  at downbeat, TR2 = stop, encoder press = track browser (512 entries, newest
  first — `sample_list_recent`). Transport bar: grey waveform, white playhead. Deck audio is
  legitimately rough ~60 s after boot (ring refill + PLL cold relock) and
  ~15 s after scrubs — judge audio only after settling (/status v1:
  healthy = i==p, E≈0, S flat). Tempo v2 (2026-07-10): analysis is EXACT
  (precision ladder: harmonic check + long-lag re-peak + sub-bin grid,
  ±0.0005 BPM on reference clicks, conf metric in sidecar v2 dver/conf,
  v1 tracks auto-upgrade on load) and FAST (raw-FatFS reads, ~1 min/track,
  full 5-min cap ~2 min; yields to playback AND ring refills). Lock
  instrument-verified: 90% of beats ±7 ms, +1.2 ms/min slip
  (tools/analyze_drift.py + tools/make_clicktrack.py are the rig).
  Archived: bin/deck-v2 (deck-v1 = pre-precision baseline).
- `machine_freesound` — silent web-driven utility: freesound search/preview
  download (`/fs/search`, `/fs/get`, `/fs/state`) and direct MP3-URL import
  (`/fs/fetch`), decoding to `usr/` (mono→stereo expand, sidecar). Auth behind
  `fs_auth` (OAuth-ready). Rejects non-44.1 kHz MP3s (no resampler).
- `main/machine_stub.c` — silence; the unloadable-proof + safe fallback

**Shared core services** (factored out of duplication):
- `components/machine/clock.{h,c}` — the ONE clock input stack (2026-07-13
  unification): `beatclock_t` detector (median ring, octave/spurious guards,
  faster-clock escape) + `clockin_t` conditioned front-end (floor-tracked
  Schmitt, ppb-scaled sanity gates, AC-tail ghost gate, raw-fire diagnostics).
  ALL clock consumers — sampler3, deck, tracker, looper, glitch — go through
  `clockin_block()`; no machine carries a private Schmitt or feeds raw CV to
  the detector anymore. `clockin_set_ppb()` on a real ppb change drops the
  lock for a clean 2-pulse relock. Deck + tracker lock quality re-verified by
  Scarlett A/B capture after the migration.
- `components/util/sample_ram.{h,c}` — `sample_list()` / `sample_load()` +
  `sample_list_shared()` (one sorted 224-entry browser list shared by
  slicer/granular — per-menu `[32]` caps silently hid fresh uploads)
- `components/util/sample_list_recent.c` — `sample_list_recent()`: the DATED
  browser walk, 512 entries in PSRAM, NEWEST FIRST, evicting the oldest when
  full (sampler3 + deck + drums). Its own translation unit because raw FatFS
  (`ff.h`) and VFS (`dirent.h`) both typedef `DIR`.
- `components/util/svf.{h,c}` — the Chamberlin state-variable filter, ONE copy
  (it had been hand-written three times: deck, looper engine, looper bounce).
  `svf_step()` gives lp/bp/hp taps; the caller keeps the coefficient slew and
  the clamp ceiling, which is what made porting deck + looper bit-identical.
  NOTE: `components/util` globs its sources — a new file there needs an
  `idf.py reconfigure` before it links.
- `components/util/sd_lock.{h,c}` — global SD-bus mutex (see Code rules)
- `components/util/mp3.{h,c}` — async decode + `decodeMP3FileSync()` (reports
  channels + samprate; the decoder does NOT resample)

**Teleremote (always-on core, NOT a machine):** `/remote/*` endpoints +
"Remote" web tab — encoder events into the UI queue, soft trigger pulses
(`audio_remote_trig()` pulls the active-low bits in the audio task), machine
switch (`EV_REMOTE_MACHINE`, same path as the front panel), and a generic
machine-settings editor (`/remote/params` = the machine's preset JSON through
`preset_load` + autosave). Gated by **System→Settings→Remote** (persisted as
`settings.remote` in CONFIG.JSN, applied live via `rest_remote_enable()`).

**Roadmap status:** all phases of
`/Users/arlo/.claude/plans/mighty-percolating-spindle.md` are BUILT and
hardware-verified (drums, web-URI hook, freesound, import routes, teleremote).
**Deck roadmap (Arlo, 2026-07-07):** polish the SINGLE Deck first (hw-verify
sync feel + analysis quality, tune PLL constant / detection) as a dedicated
full-control machine; THEN build a **separate dual-deck machine** (two
synced decks + equal-power crossfade on knob6/CV, quantized deck starts)
cherry-picking the proven parts. Do NOT fold dual into Deck — they stay
separate machines by design. Resources check out: 2x6s rings ~2.1MB PSRAM,
SD dual-stream is the classic sampler's proven load.
Still open: Freesound OAuth2, looper overdub, granular position-CV, glitch
grid-align, sampler2 removal (gated on sampler3's full hardware verdict),
sampler3 v2 leftovers (ADSR, delay, web upload-to-track; CV matrix + crop SHIPPED 2026-07-12).
Sampler3 SHIPPED 2026-07-12 (the former "deferred fork" roadmap item).

## Web UI / REST

- **Web page source** is `components/rest-api/html/index.html`; it is **NOT
  auto-built** — after editing, regenerate `components/rest-api/include/index.html.h`
  with `html/convert.sh` (`xxd -i` + sed) or the change won't ship.
- **Endpoints** (`rest-api.c`): `/status` (hot 500ms poll: machine, rec, v0/v1,
  8 CV, trig bits), `/sysinfo` (IP + SD free/total + remote flag + machine list,
  on-demand — not in the hot poll), `/files` (**streamed**, name+size only, no
  sidecar reads — see the PSRAM/DMA rule), `/files/raw` (download), `/settings`,
  `/drop_sample` (upload), DELETE `/files`, `/remote/*` (teleremote, gated),
  plus the active machine's own URIs (e.g. `/fs/*`). Tabs: Files, Upload
  (**converts any audio file in-browser** via Web Audio → 44.1k stereo RAW),
  Freesound (search/Get + direct-URL fetch), Remote (monitor + controls +
  machine settings form), Settings. Device IP also appears on the on-device
  **System→Settings** screen (`wifiGetIPString` tries STA then AP).
- **HTTPS from the device** needs `.crt_bundle_attach = esp_crt_bundle_attach`
  in every `esp_http_client_config_t` — IDF 4.3 esp-tls REFUSES https with no
  verification option (the upstream freesound code was silently broken by this).

## Working with the hardware (operational)

- **Version string**: `version.txt` in the repo root sets `PROJECT_VER`
  (shown on the About page) — bump it at milestones alongside the `bin/`
  archive; without it IDF falls back to git-describe with a `-dirty`
  suffix on any uncommitted build.
- **Build**: `export PATH="$HOME/.espressif/tools/xtensa-esp32-elf/esp-2021r2-patch3-8.4.0/xtensa-esp32-elf/bin:$HOME/.espressif/tools/esp32ulp-elf/2.28.51-esp-20191205/esp32ulp-elf-binutils/bin:$PATH"; export IDF_PATH="$HOME/esp/esp-idf-v4.3"` then `idf.py build -DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- **Flash**: port is `/dev/cu.usbserial-3110`; use the esptool invocation with
  `--flash_size detect`. ALWAYS announce before flashing (it reboots the
  device). Autonomous flashing is the default during feature iteration; the
  stricter one-flash-per-explicit-go regime applies when chasing crashes or
  hardware faults (an unexpected reboot destroys evidence).
- **Opening the serial port REBOOTS the device** even with rts/dtr deasserted,
  and occasionally drops it into ROM download mode (`boot:0x3 ... waiting for
  download`) — the display freezes on its last frame, looking like a hang.
  Recover: `esptool.py -p PORT --before default_reset --after hard_reset
  flash_id`, or a power cycle. To check a running module non-invasively use REST
  (`curl http://192.168.3.227/status`; reports the active machine name). For
  boot/crash capture use the reopen-loop reader `serial_watch.py` in scratchpad.
- **Archives** (flashable snapshots + READMEs in `bin/`, each = the full
  current firmware at that milestone): `v09-dev-stable` (pre-machines),
  `m0-complete`, `sampler2-v1`, `looper-v1`/`looper-v2`, `slicer-v1`,
  `granular-v1`, `glitch-v1`, `sd-hardening-v1` (SD-bus lock + low-memory web
  fix + IP/disk/download UI), `drums-remote-v1` (drums + freesound + universal
  upload + teleremote), `deck-v1` (Deck machine + auto-analysis + transport-bar
  UI + aborted-download httpd fix), `tracker-v1`/`tracker-loop-v1` (libxmp
  module player + KO-II sequence loop + bar scrub), `deck-v2` (exact+fast
  tempo analysis, measured lock, clip-window/corner + httpd abort fixes),
  `tracker-sync-v1` (clock detector octave+ghost guards, tracker external
  sync w/ tick-based phase servo, slicer .ot import/export, deck raw-read
  analysis), `sampler3-v1` (the deck-pattern sampler rebuild: gate
  workflow, clock-synced takes, panels UI), `sampler3-v2` (the loop
  recorder complete: CV matrix, OFF/FREE/QUANT/QUANTx2 crop, phase-exact
  gapless looping w/ pre-roll crossfade + loop-start cache, 6s capture
  queue, shared PPQ clock front-end, zero-tick busy-spin sweep).
  `drums-v2` (four pads, per-pad A/B choke layers, performable knobs w/ soft-clip
  drive + retrig, master filter, half-cell Live grid; shared SVF; newest-first
  browser; grey waveform).
  `bin/<name>/flash.sh` returns to any known-good state.
  Matching dated git tags.
- **Offline backup**: `~/ctag-straempler-backups/` — dated `git bundle --all`
  (complete repo, `git clone`-able) + a copy of `bin/`. Refresh with
  `git bundle create ~/ctag-straempler-backups/ctag-straempler-$(date +%Y%m%d).bundle --all`.

## Repo / publishing

- **origin** = `tungusk/ctag-straempler` (Arlo's fork, PUBLIC). **upstream** =
  `ctag-fh-kiel/ctag-straempler` (the original — NEVER push here).
- The seven-machine work lives on **`v09-machines`** (pushed to origin with all
  archive tags). `v09-dev` is frozen at `v09-dev-stable-20260703`.
- `overhaul` is archived/broken (IDF 4.4 audio crackling) — tagged
  `overhaul-broken-audio`, do not flash. Local overhaul branches were deleted
  (origin/overhaul preserves the commit).
- **Upstream contribution**: the `char tmp[10]` stack-smash fix (generic, not
  unit-specific) was sent as PR ctag-fh-kiel/ctag-straempler#29 from a clean
  branch off upstream/master. Do NOT PR the machine work or the unit-specific
  corrections upstream — they'd break a standard board.
