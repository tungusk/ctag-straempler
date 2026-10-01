# Review fixes — report, 2026-09-30

The fixing session for the 09-29 review (`plans/code-review-2026-09-29.md`, findings
by `#id` in `plans/code-review-2026-09-29.json`). Done by Opus on Arlo's go, one
commit per theme on `v09-machines`, **not pushed**. Bench unit `.85` runs `511b9f6`.

How it was verified: by bench and measurement, not ear-test cycles (Arlo, 09-30).
Every batch was built, flashed by OTA (announced, `recording` checked), and run
through `tools/bench/review_bench.py --audio`. Fixes the bench cannot reach got a
targeted check of their own. What only a person can judge went to
`plans/worth-hearing.md` (items 5-6 and the "hands" list).

## What was fixed

| Commit | Theme | Findings | Checked by |
|---|---|---|---|
| `2943663` | O reverse is a mirror-fold (Sampler, Slicer) | #56 #63 #64 | host test on a ramp; a 300→3000 Hz chirp through Sampler captured on the Scarlett — reverse falls monotonically, one rising step (the loop wrap) in ~610 frames |
| `1c81628` | P web cards throw on missing elements | #40 #41 #42 | served page scanned: no direct id lookup missing from the markup |
| `6ef2d8a` | L Tape data loss + E buffer freed under the save | #95-#103, #98 | bench Tape test (6 punch-outs, 6 takes) |
| `46008f3` | C UI task blocks on its own queue during a switch | #27 | proof_build; bench switch test |
| `00116dc` | D FX rack stale rows / init order | #9 #12 | code only |
| `ff05e05` | A reader DMA buffers unchecked + B zero-read spin | #57 #65 #79 #89, #61 #78 | bench; low-RAM and card-pull paths not forced |
| `e83de45` | G MP3 import | #13 #14 #17 | damaged 6 s MP3 imports at 98 % length (old code: stopped at the damage); bench import test |
| `5ca0d5f` | H unclamped preset / sidecar / config values | #118 #84 #94 #31 #117 #147 | Granular `density: 1e9` reads back 120, audio runs |
| `511b9f6` | F Looper save and bounce | #104 #105 #106 #110 | record + `/looper/save` → 200, file appears |

Per-theme notes (what exactly changed, what is owed) are under each theme in the
triage file.

## Tier 2 (same day, after the report above was first written)

| Commit | Theme | Findings | Checked by |
|---|---|---|---|
| `f87f055` | J JSON files rewritten unsafely | #20 #24 #18 #29 #86 | bench autosave test |
| `49ebd64` + `476df9f` | K settings not saved / wiped by partial updates | #162 #166 #167 #168 #169 #164 | bench; PSRAM measured across an aborted Keys load (the follow-up's leak) |
| `94539b3` | M recording service and OTA | #37 #4 #1 #7 | bench |
| `842d236` | N import, upload, file handling | #137 #36 #22 #182 #38 #138 #184 #52 #123 #124 #133 #47 | long module name -> 400; re-upload replaces; cross-folder twin -> 409 |

The first tier 2 bench had Looper landing on Stub (its 4th 705 KB PSRAM track
would not allocate). Cause found and fixed in `476df9f`: Keys' new unloaded-zone
list outlived the machine after a switch away mid-load and pinned PSRAM. A bisect
over the tier 2 commits (each flashed and measured at boot) showed no boot-time
split; after the fix the full bench passes twice, the second with audio.

New, pre-existing, not fixed: with Deck or DoubleDecker active the internal DMA
buffer for reading AUTOSAVE.JSN cannot be allocated, so autosave skips — logged
under theme J.

## Tier 3

| Commit | Theme | Findings | Checked by |
|---|---|---|---|
| `716553e` | Q web Apply / loaders | #176 #177 #178 #179 #45 #44 #163 #125 | bench; page items by a Sonnet helper, diff reviewed, `node --check` |
| `a0d82c1` | S long-filename leftovers, query decoding | #39 #43 #183 #181 #185 #131 #19 #113 | long rename, download name, `%2D`, delete-404 on the unit |
| `3a045c3` | T Drums | #73 #74 #75 | bench |
| `1069aa3` | X sample loading | #15 #21 #16 | bench |
| `6678b95` | Z zero-tick waits, broadcast | #150 #2 #5 #152 #153 | short trig spans 2 ticks |

Final bench on `6678b95` (`--audio`): 0 fail, one known Keys zone-load note.

## Bench

- Final run on `511b9f6`: **0 fail**. Click candidates all match the 09-16 runs
  (Radio switch tails, Keys zone-load attack, Tape near-zero steps, the ~555 ms
  note after a switch away from Sampler).
- One earlier run had a broken pipe in the import test right after the stall test;
  it did not reproduce.
- The Scarlett was recalibrated after Arlo moved the gains: module full scale at
  -5.0 dBFS, L/R within 0.6 dB, floor -83 dBFS (`tools/bench/calib.json`).

## Found along the way, not fixed

- **An upload while Tracker is active never converts.** The import task needs a
  20 KB internal stack; under Tracker the largest internal block is 19,456 B, so
  the upload's kick fails silently and `POST /import` answers "busy". Logged under
  theme G. `review_bench.py --only import` trips it (its snapshot pass ends on
  Tracker) — run import inside a full run.
- **Tape: an edit since the last save is lost on a reboot.** Leave and load save
  it; a power cut does not. Saving per edit would mint a `CUT_` per press. Left as
  is, Arlo's call.
- **Bench traps:** the scripts default to the old host (`--host 192.168.3.85` /
  `STRAEMPLER_IP`); `calib.py` needs Keys active; Sampler's `/remote/params`
  ignores a write without `"s3v":2`; a Sampler gate held ≥ 1 s arms recording.

## Next

Not yet done: the tier 3 themes outside Fable's list — R (web page, other), U (Sampler), V (Deck/DoubleDecker), W (FX and DSP), Y (Keys/Synth/Looper/Radio clamps), Menu and display — and the two new findings (import task under Tracker, AUTOSAVE read under the decks).
