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
  Sampler / Sampler2 / Looper / Slicer / Granular / Glitch / Drums / Deck /
  Freesound / Stub. **Stub is HIDDEN from the System→Machine selector**
  (skipped by name in `machine_sel_def_handler`, `menu.c`) but stays in the
  registry as fallback + proof target; the selector uses a parallel
  `machines[]` array so the hidden entry doesn't desync the on-screen index.
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
- `machine_sampler` — classic, byte-identical, frozen fallback
- `machine_sampler2` — `s2_`-prefixed fork: crop mode, signed CV matrix
  amounts, CV-addressable start/length, min-loop guard
- `machine_looper` — 4-track clock-synced RAM looper, save-to-library, per-track BP filter
- `machine_slicer` — one stereo sample, grid OR transient slicing + a
  sensitivity dial-in screen
- `machine_granular` — 16-grain cloud over a mono sample (raised-cosine grains)
- `machine_glitch` — live-input stutter/beat-repeat (no SD), clock beat-sync
- `machine_drumsampler` ("Drums") — up to 8 one-shot mono RAM pads. Direct
  mode: per-pad routable CV trigger (`Trig In`) through a floor-tracking
  Schmitt detector (absolute thresholds fail on bipolar/1V-oct idle levels)
  with a Low/Med/High `Sensi` setting (knobs 5/8 halve patched CV); CV-select
  mode: TRIG1/2 fire the pad addressed by a selector CV. Declick ramps +
  retrigger fade; 2x2 pad grid in 4-voice mode, 4x2 in 8-voice; R/G/B hit flash.
- `machine_deck` ("Deck") — tempo-syncing track player: streams long
  usr/*.RAW from SD through a PSRAM ring (reader task; process() never
  touches SD), varispeed playback phase-locked to the external CV clock
  (pulse-level PLL, so clock mult/div — 1/4..4 pulses per beat — works),
  offline BPM + beat-grid detection (onset flux autocorrelation) cached as
  "bpm"/"grid" in the track's JSN sidecar. TR1 = restart at downbeat.
- `machine_freesound` — silent web-driven utility: freesound search/preview
  download (`/fs/search`, `/fs/get`, `/fs/state`) and direct MP3-URL import
  (`/fs/fetch`), decoding to `usr/` (mono→stereo expand, sidecar). Auth behind
  `fs_auth` (OAuth-ready). Rejects non-44.1 kHz MP3s (no resampler).
- `main/machine_stub.c` — silence; the unloadable-proof + safe fallback

**Shared core services** (factored out of duplication):
- `components/machine/clock.{h,c}` — `beatclock_t` CV clock detector (looper + glitch)
- `components/util/sample_ram.{h,c}` — `sample_list()` / `sample_load()` +
  `sample_list_shared()` (one sorted 224-entry browser list shared by
  slicer/granular/drums — per-menu `[32]` caps silently hid fresh uploads)
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
Still open: **Sampler3** (deferred fork), Freesound OAuth2, looper overdub,
granular position-CV, glitch grid-align. Original-import machine dropped
(machine_sampler already IS the upstream v0.9 engine).

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
  fix + IP/disk/download UI). `bin/<name>/flash.sh` returns to any known-good
  state. Matching dated git tags.
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
