# Handoff addendum — 2026-07-16 evening session

Continues `handoff-20260716.md`. Everything below is **built, flashed,
machine-verified, committed, tagged, pushed, and archived** (`bin/<tag>`).
Module last ran **`icepush-v1-20260716` on ota_0**, parked on Deck, then
**powered down by Arlo** at end of session.

## Shipped this evening (in order)

1. **preset_store + Keys patches** (`keys-patch-v1`) — shared
   `components/util/preset_store.{h,c}`; Synth ported (same on-disk format);
   Keys got Save/Load Patch + browser (`usr/keys/PAT_NNN.jsn`). Round-trip
   verified on hw via teleremote (blind-driven menus + /remote/params).
2. **MP3 broadcast** (`broadcast-mp3-v1`/`v1a`) — vendored shine 3.1.1
   (LGPL, `components/shine`, state in PSRAM, -O2→-O3); `:8000/live.mp3` =
   96k MONO icecast-style; `/in` + `/in.mp3` tap the LINE INPUT (bridge mode).
   Measured (enc_us in /bcast/state, budget 26.1 ms): idle ~18 ms, deck ~13 ms
   = realtime; **radio playing ~39 ms = NOT realtime** (PSRAM-cache thrash) —
   documented limitation. Stereo never fit (~34 ms, cache-bound).
3. **Shadow framebuffer** (`shadow-fb-v1`/`v1a`) — GRAM readback is dead on
   this unit, so tftspi.c write-through hooks keep a 230 KB PSRAM copy;
   `/screenshot` serves it (~0.8 s/shot); web SCREEN card (Remote tab).
   ⚠️ The driver change lives in the **ESP32_TFT_library SUBMODULE** — fork
   `tungusk/ESP32_TFT_library`, branch `shadow-framebuffer`; `.gitmodules`
   repointed. **Claude: use /screenshot for ALL UI eye-tests** (drive with
   /remote/event, fetch BMP, sips→png). Menu-unify VISUAL pass done this way:
   all 11 Setup pages clean; only defect found was a Keys ENV overlap (fixed).
4. **Radio resampler** (`radio-resample-v1`) — streaming Catmull-Rom between
   helix and the ring; accepts 8k–48k stations. Verified: hosted 48k 440 Hz
   tone → bounce → Goertzel read **440.0 Hz exact**; 44.1k passthrough clean.
   Also `tools/ota.sh` now aborts loudly on silent device / non-ok POST.
5. **Icecast push** (`icepush-v1`) — module as SOURCE client: /ice/start|stop|
   state + web PUSH block; config persists in `usr/ICECAST.JSN`; same shine
   encoder (factored `bc_stream_mp3`); reconnect w/ backoff; mutually
   exclusive with :8000 listeners (503). Verified vs `tools/mock_icecast.py`
   (35 s @ 11986 B/s, real deck audio in the capture). **Real icecast server
   test pending — no server yet; plain SOURCE only, no TLS.**

Plus web Remote polish v2→v5: vertical CV faders inside the meter columns
(slim thumbs, meter height), CLK tag replaces the % readout, bare hold
checkboxes, bounce/broadcast cards at the bottom, collapsible SCREEN card,
top-aligned input strip.

## Lessons that went into CLAUDE.md / memory tonight
- **Internal heap ≈ 0 while Radio plays**: 4.6 KB malloc fails, OTA answers
  "oom" — stop radio before `ota.sh`; put codec/network buffers in PSRAM.
- `version.txt` must not contain `#` (CMake comment → parse error).
- `httpd_query_key_value` does NOT url-decode (`urldecode_inplace` helper).
- CSS: `input[type=range]` (0-1-1) outranks a class selector — the
  short-fader bug.

## Ear-test queue (unchanged, Arlo at the rack)
Keys engine + sustain loop; patch save/load feel; menu press-cycle tedium
verdict (Clock Src rows); broadcast listens (out + line-in bridge); Deck ppb
is at 4/beat by Arlo's hand (Scarlett testing) — his setting, leave it.

## Next build candidates
Keys v2 (gated on ear verdict), MP3-player machine, #13 boot-logo hold,
#12 on-device Editor UI, real-icecast verification, Freesound OAuth,
looper overdub / granular position-CV / glitch grid-align (build on a
listening day).
