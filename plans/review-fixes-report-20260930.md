# Review fixes — final report, 2026-09-30

The fixing session for the 09-29 review (`plans/code-review-2026-09-29.md`; findings by
`#id` in `plans/code-review-2026-09-29.json`). Done by Opus in one day on Arlo's go: every
theme A–Z plus menu/display, one commit per theme on `v09-machines`, pushed at the end.
The checks still owed by ear and by hand are in **`plans/test-list-20260930.md`**.

## Summary

- **All 26 themes fixed**, about 150 findings, in 34 commits from `2943663` to `d69c3ed`.
  Four findings are deliberately left (below).
- **Verified by bench and measurement, not ear-test cycles** (Arlo, 09-30). Every batch:
  build, `proof_build.sh` when core code changed, OTA to `.85` (announced, `recording`
  checked), full `tools/bench/review_bench.py --audio`. Fixes the bench cannot reach got
  their own check on the unit (listed per theme).
- **Three problems found by benching, all fixed:** a PSRAM leak introduced by the K fix,
  lifetime PSRAM buffers that split the pool when first used mid-session, and the two
  findings below (import under Tracker, AUTOSAVE read under the decks).
- Web page work (themes P-page, Q-page, R) went to Sonnet helpers per Arlo's model rule;
  every diff was read and the page's script passes `node --check`. The page itself was
  never clicked through (Chrome extension offline) — it is on the test list.

## Fixes by theme

| Commit | Theme | Findings | Checked on the unit |
|---|---|---|---|
| `2943663` | **O** reverse is a mirror-fold (Sampler, Slicer) | #56 #63 #64 | 300→3000 Hz chirp through Sampler on the Scarlett: reverse falls smoothly, one rising step (the loop wrap) in ~610 frames; host test on a ramp |
| `1c81628` | **P** web cards throw on missing elements | #40 #41 #42 | served page scanned: no lookup of a missing id |
| `6ef2d8a` | **L** Tape data loss, **E** buffer freed under the save | #95–#103, #98 | bench Tape test |
| `46008f3` | **C** UI task blocks on its own queue during a switch | #27 | bench switch test |
| `00116dc` | **D** FX rack stale rows / init order | #9 #12 | code only |
| `ff05e05` | **A** reader DMA buffers unchecked, **B** zero-read spin | #57 #65 #79 #89 #61 #78 | bench (low-RAM / card-pull paths not forced) |
| `e83de45` | **G** MP3 import | #13 #14 #17 | damaged MP3 imports at 98 % length |
| `5ca0d5f` | **H** unclamped preset/sidecar/config values | #118 #84 #94 #31 #117 #147 | Granular `density: 1e9` → 120, audio runs |
| `511b9f6` | **F** Looper save and bounce | #104–#106 #110 | record + `/looper/save` → 200 |
| `f87f055` | **J** JSON files rewritten unsafely | #20 #24 #18 #29 #86 | bench autosave test |
| `49ebd64` `476df9f` | **K** settings not saved / wiped by partial updates | #162 #164 #166–#169 | PSRAM measured across an aborted Keys load |
| `94539b3` | **M** recording service and OTA | #37 #4 #1 #7 | bench |
| `842d236` | **N** import, upload, file handling | #137 #36 #22 #182 #38 #138 #184 #52 #123 #124 #133 #47 | 36-char module name → 400; re-upload replaces; cross-folder twin → 409 |
| `716553e` | **Q** web Apply / loaders | #176–#179 #45 #44 #163 #125 | bench |
| `a0d82c1` | **S** long-filename leftovers, query decoding | #39 #43 #183 #181 #185 #131 #19 #113 | long rename; download named `.WAV`; `%2D` resolves; delete of nothing → 404 |
| `3a045c3` | **T** Drums | #73 #74 #75 | bench |
| `1069aa3` | **X** sample loading | #15 #21 #16 | bench |
| `6678b95` | **Z** zero-tick waits, broadcast server | #150 #2 #5 #152 #153 | a 5 ms soft trig spans 2 ticks |
| `cd4574a` `220a59f` | **R** web page, other | #46 #48–#51 #53–#55 | `node --check` |
| `cce4dab` | **U** Sampler | #58 #59 #60 | bench |
| `8b71683` | **V** Deck and DoubleDecker | #82 #92 #80(half) #83 #81 #91 #90 #87 #93 | bench |
| `23ce22b` | **W** FX rack and DSP | #11 #171 #170 #8 | bench |
| `3bed9ce` | **Y** Keys, Synth, Drums, Looper, Radio, Tracker, Glitch, Editor, Freesound | #67–#69 #71 #72 #76 #77 #119 #109 #107 #111 #112 #114–#116 #108 #121 #122 #128–#130 #33 #127 #132 | bench |
| `945e367` | **Menu and display** | #26 #32 #30 #62 #88 | bench |

## Found by benching, fixed

| Commit | What |
|---|---|
| `476df9f` | My K change kept a list of unloaded Keys zones (cJSON, in PSRAM) after Keys stopped. It pinned PSRAM, and Looper's 4th 705 KB track then failed → Stub. Freed on stop. |
| `437f67d` | **Upload under Tracker never converted:** the 20 KB import task could not be created (largest internal block ~19 KB). The kick is now retried from the UI's 1 s poll and `POST /import` says "no RAM", not "busy". **AUTOSAVE skipped under Deck/DoubleDecker:** reading the ~5 KB file needed one internal DMA block; it now reads through a 1 KB DMA bounce into PSRAM. |
| `f28ab3a` `e1a2d5b` | **PSRAM split by lifetime buffers.** The `/files` sidecar cache and folder index, the two browser lists and the beat-listener rings (~100 KB together) were allocated on first use and never freed, so they landed between machine slabs. On the bench Deck's 1.4 MB ring and Looper's 4th track then failed → Stub. They are now taken at boot, right after the SD mount. After a full bench PSRAM is within 1.5 KB of boot. |
| `77c31cd` | Looper and Deck retry their big PSRAM allocation up to 5 × 100 ms at start: the machine just stopped can still be releasing memory from its own task. |
| `d69c3ed` | **Screen flashed twice on a machine switch** (Arlo): the #9 fix re-entered the page after every web preset post, a second full repaint right after the bind. Now only off the main/live page. |

## Left on purpose

- **#10** (FX rack): the CV-modulated value is briefly visible to save and the menu. The
  fix moves CV into a per-effect offset inside every effect — a design job, best done
  with ears on the result.
- **#80, second half** (Deck): a second loop move before the first commits maps frames
  against the wrong pending window. Low priority; needs the window protocol reworked.
- **#85** (Deck): 32-bit play counters wrap after ~27 h of continuous play.
- **#6** (trig timestamps): plausible only, one chance in ~71 minutes of a torn read;
  fold into the next edit of `trig_rising_bits()`.
- **Tape:** an edit since the last save is lost on a reboot (leave/load save it).
  Saving per edit would mint a CUT_ per press — Arlo's call.

## Still intermittent — watch

- **Looper landing on Stub** once in a few full benches, always in the fast URI switch
  ring (Freesound → Radio → Editor → Synth → Looper). It needs four separate 705 KB PSRAM
  blocks out of ~3.7 MB, so any short-lived allocation in the wrong place breaks it. The
  boot reserves and retries made it rare (0 in the last 4 full runs); its failure log now
  prints free/largest PSRAM, so the next one on serial says what the pool looked like.
  The real fix is one contiguous 2.8 MB slab taken at boot for Looper — a design change.
- **SD card write timeouts** (`sdmmc_write_blocks failed (263)`, ESP_ERR_TIMEOUT) on two
  serial runs, during the import test's upload burst; the firmware refused the upload
  correctly ("SD write failed"). 27 GB free, so not space — worth trying another card or
  a card check. Probably the morning's one-off broken pipe in the import test too.

## Bench state

- Scarlett recalibrated after Arlo moved the gains: module full scale -5.0 dBFS, L/R
  within 0.6 dB, floor -83 dBFS (`tools/bench/calib.json`).
- Final run on `d69c3ed`: **0 fail**, 7 notes. Radio plays (Groove Salad 128 k) on the
  new connect path.
- Click notes on the final runs are the known kinds from 09-16 (Radio switch tails, the
  Keys zone-load attack, near-zero Tape steps, ~540 ms after a switch away from Sampler).
- **Bench traps learned today:** the scripts default to an old host (pass `--host
  192.168.3.85`, `STRAEMPLER_IP=` for calib); `calib.py` needs Keys active; Sampler's
  `/remote/params` ignores a body without `"s3v":2`; a Sampler gate held ≥ 1 s arms
  record; `--only import` used to fail by itself (now fixed by the retry); a fresh boot
  hides PSRAM problems — measure `/sysinfo` psram `big` after a full bench.

## Housekeeping not done

- `CLAUDE.md` still says "ids ≤ 8 chars / LFN OFF", "svf is Chamberlin" and "no /reboot
  endpoint" — all stale (handoff note). Left for the next docs pass.
