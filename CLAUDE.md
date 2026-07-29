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

**No reliable TFT readback on this unit — screenshots come from the PSRAM
SHADOW FRAMEBUFFER.** The ILI9341 GRAM read path (`read_data`/`TFT_RAMRD`,
0x2E) returns ALL 0xFF at runtime — MISO idles high, the panel drives nothing
back. **`find_rd_speed()` does NOT prove readback works** — a total read
failure silently returns its 1 MHz fallback and the boot log looks normal.
So `GET /screenshot` (shipped, `shadow-fb-v1`) serves a 24-bit BMP from
`tft_shadow`, a 320×240 write-through copy kept by draw hooks in tftspi.c.
The shadow is **LAZY** (a 230 KB boot alloc starved libxmp): the first call
allocates it, kicks a full redraw, and returns **503 Warming** — retry for a
complete frame. `components/util/disp_lock.{h,c}` (display-bus mutex; UI task
holds it around each `menuProcessEvent`) is taken per-row against tearing.
Use /screenshot for eye-tests instead of guessing at UI state.

**Audio block size:** 64 samples per I2S DMA block at 44100 Hz (~1.45ms per audio loop tick).

**I2S DMA depth — `dma_buf_len`, NEVER `dma_buf_count` (2026-07-27).** The DMA
shipped with `4 x 32 = 128 frames = 2.9 ms` of slack, which is less than one HTTP
request needs: measured on the analog output, **~28% of requests to the module
produced an audible discontinuity**, and with the web UI polling `/status` twice
a second that is a glitch every ~1.6 s. A ping flood does NOT do it (so it is not
the radio, and not the size of the TX burst) — it is httpd's request handling.
Now `4 x 96 = 8.7 ms`, which cuts the glitch rate under a 2 Hz poll from 0.644/s
to 0.078/s. **Reaching the same depth as `12 x 32` instead makes KEYS AUDIBLY
SCRATCHY** (its own slew ceiling goes 0.027 -> 0.142 on a sustained note), so the
harm is the DESCRIPTOR COUNT, not the buffering; `4 x 192` and `8 x 32` are also
clean. **Validate any change here ON KEYS** — it streams its sample from PSRAM
and is the host that shows this. `tools/bench/sweep_dma.py` patches, builds,
flashes and measures both metrics per configuration in one command.

**`aus` / `auspk` / `ausgap` CANNOT see a DMA underrun.** `i2s_write` uses
`portMAX_DELAY`, so after the hardware runs dry the DMA is empty, the write
returns immediately and the audio loop simply catches up — the block-to-block
interval barely moves even though audio was lost. All three meters looked
innocent through every one of the glitches above. A meter that times the TASK
cannot see the HARDWARE running dry; after deepening the buffer `ausgap` reads
higher, which is lateness being absorbed rather than dropped.

## Code rules

**Sample pool formats (2026-07-13)** — the pool speaks THREE containers via
`components/util/sampfile.{h,c}` (+ `sampfile_f.c` FatFS twin): headerless
`.RAW`, `.WAV` (PCM 16-bit/44.1k, mono or stereo), `.AIF/.AIFF` (same, big-
endian). Ids stay extension-less everywhere; `sample_resolve()` maps id →
file (.RAW wins ties). The probe sniffs MAGIC, not extension; anything else
(24-bit, 48k, compressed) is rejected with a reason string — never silence.
All conversion (header offset, mono expand, byteswap) happens in READER
tasks/UI loaders; audio tasks only ever see native int16-stereo frames.
Recordings and looper saves are written as `.WAV` (sampwav_start/finish;
power-cut takes self-heal at probe). CONVERT-ON-IMPORT
(components/util/sampimport.{h,c}): MP3 (helix) and convertible WAV/AIFF
(8/24-bit, float, any rate 8-96k) become native .WAV ONCE — streaming cubic
resampler, source REPLACED by the twin; POST /import scans the pool
(background task, 20 KB stack — helix needs it), uploads auto-kick it. MP3
sniff = ID3 or THREE chained frame headers (clipped audio fakes fewer). Take numbering = ONE readdir pass —
per-index stat probing with a missing extension walks the whole FAT dir per
call and starves the capture queue (bench-caught: 1804 dropped chunks).

**FatFS paths only** — file paths passed to `f_open()` must NOT have a `/sdcard` prefix. VFS-style paths (`/sdcard/...`) cause an `f_open` abort crash. Use bare paths like `usr/REC_0001.RAW`. (VFS `fopen()` is the opposite — it DOES want `/sdcard/usr/...`.)

**Sample ids must be ≤ 8 chars — the card is FatFS 8.3, LFN OFF.** A base name
over 8 chars is an invalid 8.3 filename and `fopen`/`f_open` rejects it with
EINVAL (errno 22). This is why every id in the system is short: recording uses
`REC_NNNN`, the web upload truncates names to 8, and generators use
`<PFX>NNNN`. When you MINT a new id (editor outputs, bounces, derived takes) keep
it ≤ 8 chars — do NOT append `_<tag>` to an existing (already up-to-8-char) id.
`sample_next_index("XX_")` gives a safe `XX_NNNN`.

**Machine SD home folders (2026-07-17)** — the pool is split into per-machine
homes: `SF_DIRS[]` in sampfile.c = `usr`, `usr/REC`, `usr/LOOPS`,
`usr/SLICES`, `usr/DRUMS`, `usr/KEYS`, `usr/TAPE` (7 dirs; boot migration
moved legacy files; `SAMPLE_DIR_*` constants in sample_ram.h select a home).
**GOTCHA: the folder indices thread through several hand-parallel arrays**
(sampfile.c / sampfile_f.c / sample_ram) — adding a folder means updating ALL
of them in lockstep; a bare count bump reads out of bounds = corruption.
(2026-07-18: KEYS/TAPE were missing from `SF_DIRS` — Tape takes silently
landed in usr/ root.)

**SD bus is serialized by `sd_lock`** (`components/util/sd_lock.{h,c}`) — a global recursive mutex that EVERY SD I/O burst must hold: the audio raw-FatFS reads, REST file serving, recording writer, config/JSON, `sample_ram`. Acquired INSIDE the per-voice `file_mutex` (consistent order → no deadlock), released between bursts. The raw audio read path bypasses the esp_vfs_fat lock, so without this it races VFS I/O and wedges/corrupts the card. Don't add SD access that skips `sd_lock`.

**SDMMC DMA can't target PSRAM** — read SD data into INTERNAL DMA-capable RAM (`heap_caps_malloc(sz, MALLOC_CAP_DMA)`), never a PSRAM buffer, or `sdmmc_read_blocks` fails `ESP_ERR_NO_MEM (257)` under memory pressure (its internal bounce-buffer alloc fails). This bit `readJSONFileAsCJSON` (empty web browser) — reads into PSRAM failed for every sidecar. Internal RAM is tight (~250 KB total); prefer STREAMING responses (see `/files`) over building big buffers, which also OOMs.

**No core pinning for tasks that read files** — pinning reader tasks to core 0 causes WiFi preemption and produces constant clicks on both voices. Leave file-reading tasks unpinned.

**Network tasks must start AFTER `initWifi()`.** `initAudio()` runs BEFORE
`initWifi()` (see ui.c), and the TCP/IP stack + the WiFi event group don't exist
until initWifi. A task that calls `socket()` (assert: "Invalid mbox") or
`isWiFiConnected()` (assert: "xEventGroup") before then crash-LOOPS the boot. The
output-broadcast server is **LAZY and OFF BY DEFAULT** (2026-07-18: its 12 KB
internal task stack, spawned every boot, starved libxmp's 32 KB render stack —
the tracker "no RAM for render" bug): `audio_broadcast_set_enabled()` spawns or
tears down the task, called from ui.c AFTER `initWifi()` with the persisted
`settings.broadcast` (default 0). Toggles: System→Settings row, web button,
`GET /bcast/enable?on=`. When enabled it is a raw lwip socket server on
**port 8000** streaming live audio
(`http://<ip>:8000/` = output bus as stereo WAV, `/live.mp3` = output as
shine 96 kbps mono MP3 icecast-style, `/in` + `/in.mp3` = the same two taps
on the LINE INPUT — streaming-bridge mode) — deliberately NOT on the shared httpd,
whose single request task a forever-streaming handler would freeze. The MP3
encoder (vendored `components/shine`, LGPL, state in PSRAM) is realtime next
to every machine EXCEPT a playing Radio (helix+shine thrash the PSRAM cache:
~39 ms per 26.1 ms pass — diag: `/bcast/state` `enc_us`). INTERNAL HEAP IS
NEARLY EXHAUSTED WHILE RADIO PLAYS: a 4.6 KB malloc failed, and even the OTA
handler returns "oom" — stop radio before `tools/ota.sh`, and allocate
network/codec buffers from PSRAM.

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
  Sampler / Looper / Slicer / Granular / Glitch / Drums / Deck / DoubleDecker /
  Tracker / Freesound / Radio / Synth / Keys / Tape / Editor / Stub.
  ("Sampler" = the deck-pattern rebuild in machine_sampler3. The legacy
  "Sampler2" `s2_` fork was **PULLED FROM THE BUILD 2026-07-18** — sampler3
  has the hardware verdict; `components/machine_sampler2` stays in the repo
  but sits in `EXCLUDE_COMPONENTS` in the top-level CMakeLists.txt, guarded so
  `proof_build.sh`'s own `-DEXCLUDE_COMPONENTS` still wins. To resurrect it:
  re-add the registry line + drop it from EXCLUDE_COMPONENTS. The frozen
  original machine_sampler ["Sampler0"] was deleted 2026-07-12 — upstream v0.9
  remains available via git history and bin/ archives.) **Stub is HIDDEN from
  the System→Machine selector** (skipped by name in both selector paths in
  `menu.c`) but stays in the registry as fallback + proof target; the selector
  uses a parallel `machines[18]` array so hidden entries don't desync the
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
  Triggers: any encoder/button event arms a 2 s debounced save (menu.c), a
  machine switch saves immediately, and POLLED-KNOB edits (which never enter
  the UI event queue) flag `machine_state_dirty()` — a 1 s menu poll arms the
  same debounce, with a forced save every ~10 s during a continuous gesture.
  Engines must flag only on a COMMITTED value change (past the take-over/
  move threshold), never every block, or the backstop saves forever. Wired
  (2026-07-20 sweep): Drums, Tape (K6-8; K5 win_move is performance-only,
  unsaved), Synth + Keys (all four macro knobs; synth cut/res/fold + keys
  start_frac became persisted keys in the same sweep), Deck (loop-length
  ladder — llenq was persisted but knob-driven). DoubleDecker needs nothing
  (its ladder is Setup/encoder-driven).
- **Adding a machine**: new `components/machine_<name>/` (see machine_glitch as
  the smallest clean example — engine + menu + priv header + CMakeLists), add
  one line to the registry + `main/CMakeLists.txt` REQUIRES, add it to
  `tools/proof_build.sh`'s EXCLUDE list, add `M_<NAME>_*` menu IDs to
  `components/menu/include/menu_types.h`, and bump the selector cap in `menu.c`
  (`names[16]`/`machines[16]`/`n<16`) if the roster exceeds it.
- **A CRASH IS INVISIBLE OVER THE NETWORK unless you look.** The module reboots
  in a few seconds and answers again looking healthy, so an operation that panics
  just appears to have quietly done nothing. `GET /sysinfo` now reports `uptime`
  (seconds) and `reset` (`poweron`/`sw`/`PANIC`/`INT_WDT`/`TASK_WDT`/`BROWNOUT`);
  `sw` is normal for OTA and `POST /reboot`. **Read `uptime` before and after any
  operation that "does nothing"** — that is how the Freesound download was caught
  (82.2 s -> 12.3 s, `RESET=PANIC`, 2026-07-28). `POST /remote/trig` also echoes
  the raw tick count, which works as an uptime probe on older builds.
- **Task stacks: measure, do not guess.** The Freesound pipeline was given 16 KB
  and MEASURES 28 KB — two TLS sessions plus a cJSON parse plus the MP3 decoder.
  It overflowed on every run. Anything doing HTTPS wants far more stack than looks
  reasonable. `uxTaskGetStackHighWaterMark` sampled at phase boundaries turns this
  into a number (`stack_min` in `/fs/state`) instead of a crash.
- **Proof invariant**: `tools/proof_build.sh` must pass — the firmware links
  with every real machine excluded and a stub-only registry. Run it after any
  change touching core or the registry. Note it EXCLUDES the machines, so it
  never compiles a machine-only change — that needs a plain `idf.py build`.
  **NEVER run two at once.** It swaps `main/machine_registry.c` and
  `main/CMakeLists.txt` for stubs and restores them from `/tmp/proof_*.bak` in an
  EXIT trap, and both copies share `build_proof/`. A second run started while the
  first is mid-build backs up the ALREADY-STUBBED files, so the traps restore over
  each other and the working tree is left with a one-machine registry — which
  builds and boots fine, so nothing complains. Recover with
  `git checkout -- main/machine_registry.c main/CMakeLists.txt`, NOT from the
  `/tmp` backups (they are the poisoned copies).

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
- `machine_sampler2` ("Sampler2") — legacy `s2_`-prefixed fork, **OUT OF THE
  BUILD since 2026-07-18** (see Registry above). Code retained in the repo for
  reference only; it had a residual WDT race in long record sessions.
- `machine_looper` — 4-track clock-synced RAM looper, save-to-library, per-track
  BP filter. House-style UI (2026-07-13): waveform-thumbnail lanes w/ state-colored
  playhead + slice redraws, click-toggle Setup rows w/ [ value ] bracket edits
- `machine_slicer` — STREAMING slicer (2026-07-13): any-length pool sample
  (RAW/WAV/AIFF), grid OR transient OR .ot slicing + sensitivity dial-in.
  Per-slice 80 ms attack heads in one PSRAM slab (playback order — reverse
  pre-flipped), playing slice's tail streamed by a reader task into a 2 s
  ring (deck discipline; process() reads PSRAM only). Length ceiling gone;
  load runs a one-pass peaks+envelope scan (~1.5 min per 8 min of audio).
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
  guard). FX (2026-07-20): hosts the shared fxrack — FX1/FX2 generic slots on
  a dedicated FX BUS, with a per-pad wet/dry ROUTING flag (Pads page "FX"
  row; one shared chain, not per-pad inserts — slab economics; wet pads wear
  an accent-tinted border in Live while a slot is live). FX3 = the reverb for
  menu/persistence, but its PROCESS stays Drums' per-pad SEND bus
  (`fxrack_process_gen_i32` skips the rack's insert reverb; Send Tap stays a
  Setup row). Legacy master-delay presets migrate into FX1=Delay via
  fxrack_load; pads default WET so they keep their sound. Knob edits flag
  `machine_state_dirty()` so kits dialed in by knob AUTOSAVE (see below).
  PITCH (2026-07-20): per-pad chromatic repitch ±12 semitones — fractional
  read cursor (Q12 semitone LUT + linear interp, works inside the stutter
  loop), envelopes/loops stay sample-domain so tuning down stretches
  tape-style. Pads row "Pitch" (±12) + knob7 CW target "pitch" (noon→CW
  tunes 0→-12, the drum move; appended as DR_CW_PITCH so old presets' cw
  values keep meaning). Pitch is knob-owned ONLY in that CW mode — a decay
  gesture in other modes won't wipe a row-set pitch. PITCH CV: per-pad
  source row (none/CV1..8) + conditional Mode row — "+/-" (bipolar around
  mid-scale, ±12; a ±5V source on ch3) or "V/oct" (semitones above the
  channel's TRACKED idle floor, 49 counts/semi, 0..+12 — root at 0V);
  quantized, adds to the base per block so ringing pads retune live
  (preset "pcv"/"pcm"). The Pads page SCROLLS (windowed around the
  cursor, ^/v arrows in the title row; no hint line).
  Live grid: a layered pad draws as two half-cells (own dot, name,
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
- `machine_dualdeck` ("DoubleDecker") — two clock-locked decks + equal-power
  crossfade (knob6/CV), per-deck DJ filter sweeps, indexed loop-ladder knobs
  with hysteresis, auto-BPM on load (Deck's analysis engine reused). Same
  ~60 s post-boot settle as Deck before judging sync.
- `machine_tracker` — libxmp module player (MOD/XM/IT/S3M…): renders in a
  reader task with a **32 KB stack** (large modules verified — a 982 KB .IT
  loads), KO-II-style sequence loop + bar scrub, external clock sync via a
  tick-based phase servo. Voices RINGING OUT across a pattern wrap is a
  FEATURE (let-ring) — don't "fix" it by choking. Broadcast being on used to
  starve its render stack — see the broadcast-lazy note above.
- `machine_instsampler` ("Keys", 2026-07-16) — tonal instrument sampler:
  plays a pitched pool sample chromatically (CV1 1V/oct or the soft-MIDI
  bridge, TR1/MIDI gate) with sustain loop + ADSR; hosts the shared FX rack.
  Live UI: big value dials (centred value, escaping needle, label below),
  ADSR pane (dimmed unfocused), waveform with origin line, green edit
  highlights. Loads via the plain browser from usr/KEYS.
  AUTO-TUNE (2026-07-20): Setup "Auto-Tune" detects the loaded sample's
  fundamental (shared `util/pitch_detect`, over the already-resident PSRAM
  buffer — ~50 ms, no SD) and writes zone `root` + the new `fine`
  (cents-as-semitones, persisted "fn"; root alone can only land within half a
  semitone, so `fine` is what makes "in tune" reachable). "Tune on Load"
  ("atl", default OFF) runs it on every fresh load; a patch's stored root/fn
  still land on top, so hand-tuning is never stomped. The sample id's note
  name ("EP_C4", "PNOF#3") is parsed as a HINT that may only fix the OCTAVE
  when it agrees on pitch class — the recording beats the label — and stands
  alone only when nothing pitched was heard. The row reports its source
  (`C4 +3c` / `(nm oct)` / `(name)` / `!F#3` conflict / `unsure`), and a
  low-confidence verdict is shown but NOT applied. Known: stretched partials
  (piano/bell) read ~8-9 cents sharp (intrinsic to period detection — trim
  with Fine); chords tune to the chord root, often an octave down.
- `machine_tape` ("Tape", 2026-07-16..19) — single-track tape recorder/looper:
  records line-in (auto take on punch-out, card-record for >30 s takes,
  beat-quantized record), in-place crop, Rev/Norm/Fade/Clear ops, loop /
  one-shot play modes, auto-restores the last take (`tapelast`), saves
  finalized takes as CUT_/TCR_ WAVs into usr/TAPE. **FX are PRINTED on the
  way in** (record path only — dry playback, no doubling); hosts the
  clock-aware shared FX rack; Level = output master. CV MATRIX (2026-07-20,
  the shared cvmtx widget — Setup > CV Matrix): Crop In / Crop Out / Window /
  Level / Cutoff, each off/CV1..8 + bipolar amount, applied as live offsets
  (crop dests as fractions of len inside tape_eff_window; Level additive —
  full-negative CV = VCA duck; Cutoff log-domain via `tp_cut_eff`) — PLUS
  five FX destinations (FX1 A/B, FX2 A/B, Rev Mix) feeding the rack's CV
  offsets; those matrix rows RENAME with the loaded effect ("Dly Mix" when
  FX1 = delay; labels refresh on page entry via tape_mtx_refresh_labels). Live UI:
  DejaVu24 title, big state-coloured waveform (redraw throttled to ~1 Hz —
  heavy TFT redraws starve the PSRAM audio path), Rev/Norm/Fade/Crop + FX1-3
  button row (short-press cycles the slot's effect, long-press opens its
  Setup; Crop = save-crop-as-take — Clear moved to Setup only, 2026-07-20).
- `machine_radio` ("Radio", 2026-07-15) — internet radio: streams an
  icecast/shoutcast MP3 station. One unpinned task GETs the endless HTTP body
  (esp_http_client + `esp_crt_bundle_attach` → http+https), decodes with helix
  frame-by-frame (honours the `MP3FindSyncWord` offset; `INDATA_UNDERFLOW` =
  refill-and-continue, not fatal) into a 4 s PSRAM stereo ring; `process()` only
  drains the ring. Pre-buffers ~0.5 s, underrun → silence + re-buffer, ring-full
  backpressure paces the decoder to real time. v1 accepts 44.1 kHz mono/stereo,
  rejects other rates (sampimport's cubic resampler is the v2 add). Control via
  web_uris (`/radio/play?station=N|url=`, `/radio/stop`, `/radio/state`) + a
  "Radio" web tab; on-device Live page picks a built-in SomaFM station. helix
  needs `#define MIPS` before `mp3dec.h` (the project's generic-C selector) and a
  20 KB task stack. No SD in the path.
- `machine_synth` ("Synth", 2026-07-15) — no-sample sound source, v1 = a mono
  subtractive voice: polyBLEP saw<->square osc (shape morph) → reused `util/svf`
  low-pass (cutoff opened by the env) → linear ADSR VCA. Pitch on CV1 at 1V/oct
  using THIS unit's measured scale (~49 ADC counts/semitone, lifted from
  sampler2's pitch LUT), zeroed at the ch1 idle (~877) so an unpatched jack plays
  the base note; quantize-to-semitone by default. TR1 gates the ADSR (teleremote
  soft trigs too). knob6 = cutoff (30 Hz..6 kHz log), knob7 = resonance. Pure-DSP
  process(). v2 roadmap: FM + wavetable engines (single-cycle waves from the pool
  via `sampfile`), polyphony, glide, dedicated filter env.
- `machine_editor` ("Editor", 2026-07-15) — offline, NON-destructive file→file
  ops on pool samples; each writes a new derived take. Background job streams the
  source (sampfile) through a transform under `sd_lock` and writes a WAV via
  `sampwav`; process() is silent. v1 ops: normalize (two-pass peak + scale),
  reverse (tail-chunk read + flip), fade in/out (gain ramp), trim silence
  (two-pass). Control via web_uris (`/edit/apply?name=&op=&param=`,
  `/edit/state`) + an "Editor" web tab; on-device Live is status-only. Output ids
  are short generated `<PFX>NNNN` (see the 8.3 rule in Code rules). v2: crop +
  zero-crossing loop-snap (needs a crop UI), audition-while-tweaking.
- **Keys is a MULTISAMPLER** (2026-07-28): up to 8 zones in one PSRAM arena,
  mapped by NEAREST ROOT with auto-tune writing the roots — there are no editable
  key ranges, the map builds itself. Setup rows: `Zone` retargets every per-zone
  row and the Live strip; `Add Zone` appends via the browser and stays on Setup;
  `Clear Zones` drops the extras and KEEPS zone 0 (Load Sample is the full reset).
  **Live follows the SOUNDING zone**, not the edited one, so playing a note is how
  you pick which zone to edit — and the loop box on the waveform is draggable
  (one detent = one pixel; clicking into an edge ARMS looping).
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
- `components/util/sampfile.{h,c}` + `sampfile_f.c` — the sample-pool FORMAT
  SEAM (see Code rules): probe/seek/read across RAW/WAV/AIFF + id resolver +
  WAV write helpers. Every streaming reader (deck/dualdeck/sampler3) and
  `sample_load` goes through it.
- `components/util/reverb.{h,c}` — multi-mode Dattorro reverb (Room/Hall/
  Plate/Shimmer over one PSRAM tank, ~170 KB lazy slab, live cost meter);
  drums hosts it post-filter. Shimmer runaway-on-silence + fade-out fixed
  2026-07-18 (`SHIM_FB_LP` feedback low-pass, DC-block, gain/decay retune);
  reverb mode switches mute + clear the tank (no explosion).
- **`components/fxrack/` — the shared FX rack** (plan:
  `plans/fx-rack-20260717.md`): FX1/FX2 = curated generic slots
  (Off/Overdrive/Flanger/Tremolo/Delay/Filter/Band), FX3 = the reverb slot;
  ONE descriptor table + dynamic per-slot Setup menu + process path
  (`fxrack_process_i32`) + (de)serializer, hosted by Keys / Synth / Tape /
  Drums instead of per-machine copies. An `fxrack_t` is a POINTER VIEW — the
  machine owns the effect structs and hands the rack pointers. Clock-sync
  divisions come from ONE shared table (`fxrack_div_beats/_names`); machines
  that know a tempo set `.bpm` and flip an effect's `sync` on.
  `fxrack_process_gen_i32` runs the generic slots WITHOUT the insert-reverb
  stage — for hosts whose reverb is a send bus (Drums). CV MODULATION
  (2026-07-20): `fxrack_t.cv1/cv2[slot]` + `.cv_rv` are per-block bipolar
  offsets the host's CV matrix writes; applied INSIDE process as a
  push/compare-restore around each stage, so menu + preset always see the
  un-modulated base (a mid-block UI edit survives the restore). Curated A/B
  param per kind (`fxrack_cv_label` names them): OD drive/level, flanger
  depth/fdbk, tremolo depth/rate (rate = ±3 octaves log, rides ON TOP of
  sync), delay mix/fdbk, filter cutoff/reso, band base/width, reverb mix.
- **FX bricks** (`components/util/`): `fxdelay` (stereo feedback delay,
  ~690 KB lazy PSRAM slab, ms + beats time modes, ping-pong, damping),
  `overdrive` (tanh shaper, tone/asymmetry, no slab), `tremolo` (LFO
  amp / stereo auto-pan, beats-syncable), `flanger` (~1-11 ms swept
  fractional delay + feedback, lazy slab; recirculation darkened 07-18),
  `fxfilter` (LP/HP/BP + base/width band variant).
- `components/util/fxchain.h` — the FLOAT-chain convention: a hosted chain
  unpacks int32 once, runs ALL stages in float scratch, and soft-clips ONCE
  at the end — no inter-stage clamps (stacked FX used to clip at every
  seam). Machines with int32 buffers use the `*_block_i32` wrappers.
- `components/util/preset_store.{h,c}` — named machine-preset save/recall
  on SD (the Scenes roadmap item builds on it).
- `components/menu/cvmtx.{h,c}` — the shared assignable CV MATRIX widget
  (2026-07-20): N host-labeled destinations, each source (off/CV1..8) +
  bipolar amount; owns the ch1/2 floor-tracked conditioning (1V/oct jacks
  idle ~21% — synth's sy_mtx_cv01 lifted), the matrix page (label|[src]|[amt]
  rows, press cycles nav>src>amt), and "mxs"/"mxa" (de)serialization (loads
  the legacy synth/keys "msrc"/"mamt" keys transparently). Hosts: Tape,
  Synth, Keys (the latter two MIGRATED 2026-07-20 — their hand-rolled matrix
  pages are deleted). Host applies `cvmtx_val()` (-1..+1) as live OFFSETS —
  the win_move convention. Sampler3's matrix is engine-woven (per-voice
  speed/start/length) and stays bespoke. DoubleDecker keeps its own CV Map
  (channel assignment, not a matrix — don't duplicate); Deck's contextual
  loop knobs ARE its CV interface, no matrix by design. Lives in
  components/menu (the sample_browser precedent — it draws).
- `components/machine/tuner.{h,c}` — line-in CHROMATIC TUNER, the beatlisten
  shape: `tuner_push(in)` taps the core input in the audio task beside
  `beatlisten_push`, an unpinned prio-4 task runs `pitch_detect` over 4096-frame
  buffers (two whole buffers, not a ring — the detector wants a CONTIGUOUS
  window). OFF by default and lazily allocated (~16 KB PSRAM + ~8 KB scratch);
  **System > Settings > Tuner** turns it on when opened and off when left, so
  it is one branch per block otherwise. Steadiness follows the beatlisten rule:
  the note changes only after 2 windows agree, the needle slews, a few silent
  windows are needed to drop out. Detection measured ~35-40 ms/window on
  hardware, so it is RATE-LIMITED to ~4/s (analysing every 93 ms buffer costs
  ~40% of a core; `aus` 504 -> ~545-585 with the limit, back to 502 when shut).
  A tuner-specific LEVEL GATE (~-38 dBFS) + 0.5 confidence floor sit above
  pitch_detect's own offline-tuned floor — a live Eurorack input always has
  hum/bleed and will otherwise report a note at 30-60 Hz from noise. Status via
  `tuner_get_status()`, `/status` "tun" object (only while on), and
  `GET /tuner/enable?on=` (not persisted — a tuner is opened, not left on).
- `components/util/pitch_detect.{h,c}` — monophonic PITCH DETECTION (YIN:
  coarse lag sweep on a 4x decimated window, then a native-rate refine +
  parabolic fit; ~6 KB scratch, no SD/globals). Sub-cent on host tests from
  55 Hz to 1.3 kHz, refuses noise/silence. Also parses a note name out of a
  sample id (`pitch_name_hint`, reporting whether the match was isolated or
  buried in a word like "TAPE4"). NOT audio-task work (a scan is ~50 ms) —
  UI/loader context, as bpm_analysis is. Host: Keys auto-tune; the live
  line-in tuner (beatlisten-shaped service) is the queued second consumer,
  which is why the single-window entry point is public.
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
Current build queue: **`plans/roadmap-20260717.md`** (MIDI bridge SHIPPED as
midi-v1; FX pack SHIPPED; tuner + Keys sample auto-tune, scenes, Ableton Link
still queued). Drums per-pad FX SHIPPED 2026-07-20 as the lighter routing design (Arlo):
fxrack slots + per-pad wet/dry flag on an FX bus — NOT per-pad inserts (built,
awaiting hardware verify; module was offline). Tape crop CV matrix + Live
Crop button SHIPPED 2026-07-20 (the shared cvmtx widget), and drums per-pad
PITCH SHIPPED 2026-07-20 (all in the same built-not-yet-verified state).
Still open:
granular rework (Arlo flag 2026-07-18, specifics TBD — ask), Freesound
OAuth2, looper overdub, glitch grid-align, sampler3 v2 leftovers (ADSR,
delay, web upload-to-track). Sampler2 was pulled from the build 2026-07-18.
Sampler3 SHIPPED 2026-07-12.

## Web UI / REST

- **Web page source** is `components/rest-api/html/index.html`; it is **NOT
  auto-built** — after editing, regenerate `components/rest-api/include/index.html.h`
  with `html/convert.sh` (`xxd -i` + sed) or the change won't ship.
- **Endpoints** (`rest-api.c`): `/status` (hot 500ms poll: machine, rec, v0/v1,
  8 CV, trig bits, `aus` = audio-loop cost in µs — 1450 µs ≈ 100% of the
  block budget, the load meter; `auspk` = peak-hold of the same, cleared on
  read, which catches the single overrunning block a click actually is; `ausgap`
  = peak block-to-block INTERVAL, catching the task coming back LATE rather than
  running long; `sav` = duration + count of AUTOSAVE.JSN writes, which cost
  ~64 ms each; `fxpk` = peak inside the FX rack BEFORE the soft limiter, since
  the output VU reads the limiter's OUTPUT and a limiter working flat out looks
  like a moderate level). **`/status?fx=1` additionally returns per-stage FX
  meters** (`fxst.pk`/`fxst.jp` at rack input / after FX1 / after FX2 / chain
  end, plus `rv.wpk` and a NaN-guard count) — these are ARMED ON DEMAND because
  measuring is per-sample work in the audio path, and a plain `/status` must not
  arm them or the web UI would hold the instrumentation on forever.
  **Never poll while judging audio**: 4 Hz polling pushed peak block cost to
  1495-1517 µs and was audible. `/screenshot` (shadow-FB BMP — see the TFT
  section; first call 503 Warming), `/peaks?name=&n=` (n peak bytes for web
  waveform thumbnails, cost independent of file size), `/bcast/enable?on=` +
  `/bcast/state` (the :8000 broadcast, off by default), `/ws/midi` +
  `/midi/*` (MIDI bridge), `/ota` + `/ota/state`, `/sysinfo` (IP + SD free/total + remote flag + machine list,
  on-demand — not in the hot poll), `/files` (**streamed**, name+size only, no
  sidecar reads — see the PSRAM/DMA rule), `/files/raw` (download), `/settings`,
  `/drop_sample` (upload — sniffs the payload and kicks convert-on-import for
  MP3/48k/24-bit arrivals; pad appended at END, never leading), POST/GET
  `/import` (pool-wide convert-on-import scan / progress), DELETE `/files`,
  `/remote/*` (teleremote, gated), `/bounce/{start,stop,state}` (record the
  active machine's OUTPUT bus to a REC_ take — "sample the radio"; core, any
  machine, reuses the recording service fed `out` instead of line-in),
  plus the active machine's own URIs (e.g. `/fs/*`, `/radio/*`, `/edit/*`). Tabs: Files, Upload
  (**converts any audio file in-browser** via Web Audio → 44.1k stereo RAW),
  Freesound (search/Get + direct-URL fetch), Remote (monitor + controls +
  machine settings form), Settings. Device IP also appears on the on-device
  **System→Settings** screen (`wifiGetIPString` tries STA then AP).
- **MIDI bridge (midi-v1, 2026-07-17)**: web "MIDI" tab = musical typing
  (GarageBand-style computer-keyboard layout) + WebMIDI device capture →
  WebSocket `/ws/midi` (needs `CONFIG_HTTPD_WS_SUPPORT` — NOTE sdkconfig is
  gitignored, keep the flag when regenerating) with `/midi/*` per-event
  fallback → core soft-MIDI state in audio.c (`audio_midi_note_on/off`,
  held-note stack, last-note priority, `audio_midi_gate/note` + liveness
  heartbeat). Synth + Keys opt in next to their CV1/TR1 reads — exact pitch,
  no CV quantization detour. No hardware MIDI (USB impossible on this ESP32;
  a TR2-UART TRS-MIDI experiment is parked in `plans/roadmap-20260717.md`).
- **HTTPS from the device** needs `.crt_bundle_attach = esp_crt_bundle_attach`
  in every `esp_http_client_config_t` — IDF 4.3 esp-tls REFUSES https with no
  verification option (the upstream freesound code was silently broken by this).

## Working with the hardware (operational)

- **Version string**: `version.txt` in the repo root sets `PROJECT_VER`
  (shown on the About page) — bump it at milestones alongside the `bin/`
  archive; without it IDF falls back to git-describe with a `-dirty`
  suffix on any uncommitted build. **GOTCHAS**: a `;` OR a `#` anywhere in
  version.txt breaks the CMake build, and keep it under ~140 chars (it is
  embedded in `/sysinfo`; overlong trips a -Werror in rest-api). The device
  often runs code PAST its version stamp between milestones — re-OTA only to
  sync the About label.
- **Build**: `export PATH="$HOME/.espressif/tools/xtensa-esp32-elf/esp-2021r2-patch3-8.4.0/xtensa-esp32-elf/bin:$HOME/.espressif/tools/esp32ulp-elf/2.28.51-esp-20191205/esp32ulp-elf-binutils/bin:$PATH"; export IDF_PATH="$HOME/esp/esp-idf-v4.3"` then `idf.py build -DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- **Flash — OTA is the primary path now (2026-07-15).** The device runs an
  OTA-capable image (two 3 MB app slots `ota_0`/`ota_1` + otadata; see
  `partitions.csv`). Update over WiFi with **`tools/ota.sh [IP]`** (`POST /ota`
  streams the build into the inactive slot and reboots into it) — ~14 s,
  untethered, no ROM-download-mode risk. `GET /ota/state` reports the running
  slot. Still announce before flashing (it reboots). No rollback yet (a bad
  image needs serial recovery — a deliberate follow-up). A long
  screenshot/OTA/take marathon can exhaust internal RAM (`ota_begin failed`,
  audio glitches) — there is NO /reboot endpoint; recover with a power-cycle.
- **Serial flash = migration + recovery only.** Use the esptool invocation with
  `--flash_size detect` when (a) migrating the partition layout, or (b)
  recovering a device that won't boot: `bin/<archive>/flash.sh` restores a
  known-good image. NOTE the OTA layout offsets differ — a fresh serial flash of
  an OTA build is `0x1000 bootloader / 0x8000 partition-table / 0xf000
  ota_data_initial.bin / 0x20000 app` (idf.py prints the exact command).
  `bin/synth-v1` is the OLD single-app layout (app @ 0x10000) — flashing it
  reverts OTA; re-migrate to get OTA back. Autonomous flashing is the default
  during feature iteration; one-flash-per-explicit-go when chasing crashes.
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
  browser; grey waveform). Later (2026-07-14..19, see each README):
  `dualdeck-v1`/`doubledecker-v1`, `import-v1`, `convergence-v1`,
  `clock-unify-v1`, `deck-loop-v2`, `menu-unify-v1`, `synth-v1`,
  `keys-v1`/`keys-patch-v1`/`keysui-v1` (Keys machine + Live UI polish +
  redraw-throttle audio fix), `slicer-fx-v1`, `fxnav-v1`, `fx-stability-v1`
  (shimmer runaway + flanger fixes), `folders-v1` (machine SD homes),
  `broadcast-mp3-v1(a)`, `icepush-v1`, `shadow-fb-v1(a)` (/screenshot),
  `midi-v1`, `tape-v1`/`taperack-v1`/`tapeui-v1`, `tracker-ram-v1`
  (broadcast-lazy RAM fix), `wav-v1`, `looper-v3`, `streaming-slicer-v1`.
  `bin/<name>/flash.sh` returns to any known-good state.
  Matching dated git tags.
- **A KNOB CAN BE ROUTED TO ITS OWN DESTINATION — check the CV matrix first.**
  Arlo's Keys patch had `Cutoff <- CV6` at FULL amount, and CV6 *is* knob 6,
  already the cutoff control: cutoff driven twice from one source. Symptoms that
  cost hours on 2026-07-28 — remote `cut` writes snapping back to a few hundred
  Hz, an instrument sounding dead (K6 at 243 Hz with 81% resonance metered VU 1
  against 216 with the filter open), and an LFO patched to that jack sweeping the
  filter through an entire calibration run. **Before diagnosing "it sounds quiet
  or wrong", read `mxs`/`mxa` from `/remote/params` and the knob-owned params
  back.** Knob takeover also means a remote write to `cut`/`res` holds only until
  the physical knob MOVES.
- **Bench capture rig — `tools/bench/`** (2026-07-27). Records the module's
  ANALOG output through an audio interface (`sox -t coreaudio`) and drives it
  over REST. Use it in preference to `/bounce` or the `:8000` broadcast, both of
  which put load INSIDE the thing under test (a bounce's SD writes alone push
  peak block cost to 1447 µs of a 1450 µs budget on a DRY case).
  `calib.py` once after patching both outputs (and `--verify` later — it fails if
  the interface gain moved); `listen.py` to capture while a human plays;
  `detect.py` to find discontinuities; `soak.py` to hunt unattended;
  `sweep_dma.py` for I2S geometry. The detector's threshold comes from the
  material's own slew ceiling, not a guess, and is validated against a labelled
  capture kept at `~/ctag-straempler-backups/labeled-captures/`.
  **Gotchas, all learned the hard way:** sox must write `-t wavpcm` (its default
  coreaudio output is WAVE_FORMAT_EXTENSIBLE and python's `wave` refuses it);
  opening and closing the device every 30 s for a day WEDGES CoreAudio's link to
  the interface (sox starts, reports 0% input, records nothing, never exits —
  only a physical replug clears it), so `capture_blocking` detects that and cools
  down between opens; long captures wedge more readily than short ones, so work
  in ~30 s segments; and assert a take is not silent before analysing it, because
  silence still yields a fundamental, a modulation depth and a click count, all
  of them garbage.
  **Setting the interface gain: aim MODULE FULL SCALE at about -6 dBFS**, not
  whatever tone happens to be playing. `calib.py` divides the module's own `vu`
  out of the arriving level to extrapolate that. Targeting the reference patch
  itself put full scale ~8 dB ABOVE 0 dBFS (the patch ran at `lvl 0.30` with FX),
  and a capture that clips is unrecoverable — no later analysis detects or undoes
  it. Once set, LEAVE IT: `calib.py --verify` compares the extrapolated
  full-scale figure, which is the only number that separates "gain moved" from
  "the patch is quieter".
  **Hold notes with `rig.hold_note()` (MIDI), not `hold_gate()`** whenever level
  or continuity is what you are measuring — the soft gate behind `/remote/trig`
  releases early and intermittently, which reads as a signal wandering by 20 dB
  for no reason. The MIDI gate self-clears after 5 s; re-posting the same note
  refreshes it and does NOT retrigger, since the machine sees the gate stay high.
  **And check nothing is MODULATING what you are measuring.** An LFO patched to
  CV6 swept the cutoff through an entire calibration run on 2026-07-28 and was
  indistinguishable from a level fault; `calib.py` now refuses to run while
  cutoff or resonance is moving. Knob-owned params (`cut` = K6, `res` = K7) also
  reject a remote write while the knob is live, so always read them back.
- **Loading the card from a Mac — DELETE THE APPLEDOUBLE FILES.** Copying onto
  the FAT card creates `._NAME.WAV` sidecars, and they are created by the WRITE
  (not by reads), so `cp -X` and even `cat >` produce them. They end in `.WAV`,
  so the module's browser LISTS them as samples — with a 9-character base that
  FatFS shows as mangled junk like `_MOOGC~1.WAV`. Always finish a card session
  with `find /Volumes/<card> -name '._*' -delete` and eject cleanly. Worth
  clearing `.DS_Store` at the same time.
- **Naming samples for Keys: use SCIENTIFIC note names** (middle C = C4). Keys
  parses a note out of the id as an octave HINT for auto-tune, so a library that
  numbers octaves the Yamaha way (middle C = C3, e.g. the Moog Voyager set) must
  be CONVERTED on the way in or the hint fights the recording. `tools/curate.py`
  does this with `--middle-c`. The hint is now **capped at one octave** of
  correction (`keys_autotune`) — it exists to fix a sub-harmonic detection error,
  and unbounded it equally obeyed a name that was just WRONG: the card's `TSTC2`
  holds a C4 tone, so the "C2" in its name moved the root two octaves down and the
  zone played 2400 cents sharp while reporting `fine = 0.00` cents, which looks
  exactly like a confident correct verdict. A disagreement past one octave is
  reported as `TUNE_CONFLICT` and the recording wins. `tune_src` is **per zone**
  (`"ts"` in the zones array) — the instrument-level one only ever holds the LAST
  load's verdict, which tells you nothing when one zone of eight is out of tune.
- **BUILDING A MULTISAMPLE FROM A SAMPLE LIBRARY** (done 2026-07-28 from Arlo's
  Moog Voyager set; five notes G2-G6 self-tuned 5/5 and he kept the patch). Three
  things the raw files need on the way in:
  1. **TRIM.** The arena is ONE PSRAM block from a ladder (1,000,000 frames, then
     750,000, 500,000...) and in practice lands at ~750,000 = **17 s of mono
     audio TOTAL**, across all zones. Five 8 s notes is 40 s and simply will not
     fit — it is SECONDS OF MATERIAL, not zone count, that runs out first. A
     `/reboot` before building gets the best arena available.
  2. **GAIN, but the SAME gain for every note.** Per-file normalisation flattens
     the relative dynamics between zones, which is the one thing a multisample
     must not do. Find the loudest of the set and apply one fixed dB figure to all.
  3. **OCTAVE-SHIFT THE NAMES** if the library numbers middle C as C3 (the Moog
     Voyager set does) — Keys expects scientific, middle C = C4.
  Upload with `PUT /drop_sample` + a `Name:` header, which writes the bytes
  verbatim to `usr/<NAME>.RAW`; send raw **int16 stereo 44.1 kHz** (`sox ... -r
  44100 -c 2 -b 16 -e signed -t raw`). The importer sniffs MAGIC not extension,
  so a container behind a .RAW name still converts — but note `POST /import`
  SKIPS any filename starting with `IMP` (its temp-file guard).
- **Two zones with the same root: the later one is DEAD.** `keys_zone_for_note`
  takes the nearest root and breaks ties toward the FIRST, so a duplicate root
  leaves a zone occupying arena and never sounding, with nothing reporting it.
  Auto-tune produces this readily from a folder holding two samples of the same
  note. `tools/bench/zone_sweep.py` detects it.
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
