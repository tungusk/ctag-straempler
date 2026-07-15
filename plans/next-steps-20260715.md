# Next-steps queue — 2026-07-15

Reconstructed after a context clear, then built the same day. All three items
are **CODE-COMPLETE + build/proof-verified, NOT yet flashed** (test in the
evening). Nothing here touches the unverified doubledecker/beatlisten code.

Build: `idf.py build` green. Proof: `PROOF PASSED` (core links machine-less).

## ✅ 1. Drums folder — `usr/DRUMS`
A fifth first-class folder alongside pool/REC/LOOPS/SLICES.
- Central enum `SAMPLE_DIR_DRUMS=4`, `SAMPLE_DIR_N=5` (`sample_ram.h`); every
  parallel string array extended: `sample_ram.c` (dirs+names), `sample_list_recent.c`,
  `sampfile.c` (SF_DIRS), `sampfile_f.c`, `sample_browser.c` (k_fold), and REST
  `rest-api.c` (jdirs/jdir_tag/JN_DIRS, fdirs, del_dirs, mv_dirs/mv_tags/MV_DIRS,
  rn_dirs) + web chips in `index.html` (regenerated `index.html.h`).
- **Drums machine opens INTO usr/DRUMS**: new `sample_browser_enter_dir(...,dir)`
  forces the start folder; `drum_menu.c` calls it with `SAMPLE_DIR_DRUMS`.
- **Bonus fix (latent bug):** SLICES was silently missing from three sites
  (FatFS resolver `sampfile_f.c`, import scan `sampimport.c`, web delete
  `del_dirs`) — a SLICES file couldn't be resolved-by-id, convert-imported, or
  web-deleted. Added SLICES + DRUMS there and made those loop bounds derive from
  the array so they can't drift again.
- Folder is created lazily on first web-move into it (`mkdir(mv_dirs[dst])`); the
  DRUMS row shows in the browser even when empty.

**TEST TONIGHT:** DRUMS chip appears on web + on-device browser; move a sample
into DRUMS from the web (folder cell dropdown); Drums machine → load a pad →
browser opens in DRUMS; delete a DRUMS/SLICES file from web works now.

## ✅ 2. Looper Live UI — deck's visual grammar
Ported deck's transport grammar to the 4-lane view (`looper_menu.c`):
- **White playhead** (was state-colored) — transport-neutral, like deck.
- **State on a FAT box border** around each lane's bar (deck's "state on the
  border, not the fill"), replacing the old thin outline + full-lane frame.
  Colors: ARM amber / REC red / PLAY green / STOP blue / EMPTY dim.
- **Selected lane wears its number as a WHITE PLATE** (the DoubleDecker focus cue).
- Grey waveform on black canvas + existing `s_wf[]` peaks reused (no recompute);
  redraw gating preserved (chrome only on state/selection change; playhead moves
  repaint one slice — no strobe).
- Reconciliation note: looper tracks have no sub-loop window, so the box frames
  the whole bar (deck's non-looping full-width case). If a crop/sub-loop feature
  lands later, the box can shrink to it exactly like deck.

**TEST TONIGHT (by eye — use the screenshot tool!):** record loops on a few
lanes; confirm white playhead reads over the waveform; state box color tracks
ARM→REC→PLAY→STOP; selected lane's number is a white plate; no redraw churn.

## ⛔ 3. Web screenshots — SHELVED (panel readback dead on this unit)
Built `GET /screenshot` (GRAM readback → streamed BMP) + `util/disp_lock`, flashed
and tested: **came back a solid white box — every pixel byte 0xFF.** The ILI9341
MISO readback is non-functional at runtime on this hardware (see CLAUDE.md "No
reliable TFT readback"; `find_rd_speed`'s 1 MHz fallback masked it). SCREEN card
hidden; endpoint + disp_lock kept as dormant infra. To make it work would need a
PSRAM **shadow framebuffer** (draws write through to an RGB565 copy) — a
half-day job, deferred.

## ✅ 4. Faster uploads — MP3/WAV/AIFF upload as-is, convert on device
The web Upload tab decoded EVERY audio file to a 44.1k-stereo RAW in-browser
(`convertAudio`) then uploaded that — an MP3 became ~10x bigger over the
TX-power-capped WiFi (`CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER=10`). Now
device-native formats (`DEV_EXTS = mp3/wav/aiff`) upload their ORIGINAL bytes to
`/drop_sample` and ride the module's existing convert-on-import (sniffs payload
MAGIC, converts, pad-at-end keeps containers byte-exact). Only formats the module
can't decode (flac/ogg/m4a/aac/opus) still decode in-browser. Native uploads also
get a real progress bar (vs the old indeterminate convert spinner). Web-only
change; device path already proven (mp3→WAV verified in import-v1).
**(This replaced the "USB uploader" idea — on the classic ESP32, USB is a serial
bridge and would be SLOWER than WiFi for bulk data; the real win was killing the
in-browser RAW bloat.)**

**TEST TONIGHT:** drop an MP3 in Upload → it shows "`.MP3→module`", uploads at
its small original size (fast), then the IMPORT card shows it converting; it then
plays. Drop a FLAC → still "audio→RAW" (in-browser), still works.

## ✅ 5. Web instrument — perform CV/knob sliders (Remote tab)
The teleremote could configure a machine but not PERFORM one (every performance
control is a knob). Backend already existed (beatlisten agent: `POST /remote/cv`
+ `audio_remote_cv`/`s_remote_cv_val` applied over `io.cv`/`io.cv_raw` with a
timeout decay). Added the missing UI: a "Perform" card with 8 sliders that POST
`/remote/cv?ch&v&ms`. Drag = live sweep (throttled ~1/70ms, ms=300 so a moving
slider holds); per-channel **hold** re-posts within the decay to sustain;
release/untick/close = decays back to the physical knob (nothing pinned by a
stale value). FLASHED + verified: a slider drives the target CV in /status and
releases back. Testable now on the reloaded Remote tab.

## ✅ 6. DoubleDecker auto-BPM analysis (unstamped tracks can loop)
DoubleDecker refused to loop any track without a sidecar bpm stamp. Lifted the
deck's proven analyser into `util/bpm_analysis.{h,c}` (DSP-identical; the
playback gate is now a `busy()` callback, results are out-params, +
`bpm_analyze_abort()`); `deck_analysis.c` is now a thin wrapper (behaviour
unchanged). DoubleDecker kicks it on loading an UNSTAMPED track onto a STOPPED
deck (Arlo's constraint); `busy()` covers BOTH decks (a starved ring = phase
slip); one run at a time (other deck queues); on done, writes the v2 sidecar and
adopts bpm/grid live only if still loaded + stopped; reset in start(),
aborted+waited in stop(). VERIFIED on device: SPLITTIN→86.53 BPM, LANDER→79.99,
both dver 2 + grids, adopted live. Commit f67659f.
- Watch tonight: confirm a plain single-Deck track still LOCKS well (shared DSP
  was refactored — DSP is byte-identical, but ears are the real check).

## ✅ 7. Internet radio IN — new `machine_radio` (flagship pick)
New isolated machine that streams an icecast/shoutcast MP3 station. One unpinned
task GETs the endless HTTP body (esp_http_client + crt_bundle → http+https),
feeds helix frame-by-frame (honours the sync offset; INDATA_UNDERFLOW = "refill,
keep going"), decoded PCM → 4 s PSRAM stereo ring, `process()` drains it (deck
discipline). Pre-buffers ~0.5 s; underrun → silence + re-buffer; ring-full
backpressure paces the decoder to real time. v1 = 44.1k mono/stereo (other rates
rejected; cubic resampler is v2). Web tab "Radio" (station buttons + custom URL +
state) + `/radio/play|stop|state`; on-device Live page picks a built-in SomaFM
station. Registered everywhere; proof passes. VERIFIED ON DEVICE: Groove Salad +
DEF CON connected, helix decoded 128 kbps/44.1k/stereo, PLAYING, zero underruns.
Commit 3d0f9cb. v2 ideas: auto-reconnect, ICY now-playing metadata, non-44.1k
resample, persistent/editable station list, "sample the radio" (bounce the
output bus into the pool).

## ✅ 8. Synth voice machine — subtractive mono synth
New `machine_synth`: no-sample sound source. v1 mono subtractive voice —
polyBLEP saw↔square osc (shape morph, anti-aliased) → reused `util/svf` low-pass
(cutoff opened by the env) → linear ADSR VCA. Pitch on CV1 at 1V/oct (unit scale
~49 ADC counts/semitone from sampler2's LUT, zeroed at the ch1 idle so unpatched
= base note; quantize default). TR1 gates the ADSR. knob6 cutoff / knob7 res.
Setup: shape/note/quantize/ADSR/env→cut/level (preset-persisted). Registered;
proof passes. VERIFIED via teleremote: CV1 pitch + TR1 gate → clean
attack→decay→sustain, released to silence, mono out. Commit 8b9a994. v2: FM +
wavetable engines, poly, glide, filter env, pitch calibration by ear.

## ✅ 9. Audio editor S-core — offline file→file ops
New `machine_editor` (silent, web-driven): non-destructive ops on pool samples,
each writes a new derived take. Background job streams src→transform→WAV under
sd_lock. v1: normalize (2-pass peak), reverse (tail-chunk flip), fade in/out,
trim silence (2-pass). Web "Editor" tab (pick sample + op + progress);
`/edit/apply` + `/edit/state`. Registered; proof passes. VERIFIED on device:
reverse + normalize → valid same-length WAVs (RV_/NM_ outputs, deleted after
test). LESSON: card is FatFS 8.3/LFN-off → ids MUST be ≤8 chars (first attempt
`<src>_<tag>` fopen'd EINVAL); now `<PFX>NNNN` via sample_next_index. Recorded in
CLAUDE.md Code rules. v2: crop + zero-cross loop-snap (needs a crop UI).

---

## 10. _(more — to be recalled)_
Arlo had more ideas last night that were lost to the clear; add them here as they
resurface. Separate/older backlogs: `ideas-round2-20260713.md`,
`plans/roadmap-speculation-20260714.md` (B1–B9).
