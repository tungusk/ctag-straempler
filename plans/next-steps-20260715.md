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

---

## 5. _(more — to be recalled)_
Arlo had more ideas last night that were lost to the clear; add them here as they
resurface. Separate/older backlogs: `ideas-round2-20260713.md`,
`plans/roadmap-speculation-20260714.md` (B1–B9).
