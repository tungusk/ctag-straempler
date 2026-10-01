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

Tier 2: J (JSON writes), K (settings not saved / wiped), M (recording + OTA),
N (import, upload, file handling). Then tier 3: Q, S, T, X, Z.
