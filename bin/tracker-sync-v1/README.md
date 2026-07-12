# tracker-sync-v1 (v1.1 refresh)

The clock-hardening + tracker-sync milestone: every tempo-following machine
now survives dirty clock signals, and the Tracker locks to the external
clock for the first time. **v1.1 refresh = commit `a1aa234`** (tag
`tracker-sync-v1.1-20260712`; originally cut at `cf95860`). Flash with
`./flash.sh [PORT]`.

## v1.1 refresh (measurement-session night)

- **Deck lock, final numbers: beats land +2.0 ms from the pulse, 1.08 ms
  std, 100% within ±7 ms** (240-beat capture) — fixed-time lock lead
  (13.1 ms commanded) tuned by two-channel capture.
- Detector lock-chain fixed: lower-middle median (median-of-2 took the
  LARGER interval — one missed boot pulse crowned a half-tempo lock the
  ghost guard then cemented) + faster-clock escape via raw edge cadence
  (rate-ups relock in ~0.75 s instead of deadlocking).
- /status v1 debug gains g/L/n (ghost-run / locked / ring-fill).

## New since deck-v2

**Shared clock detector hardened** (all machines — deck, tracker, looper,
glitch — inherit):
- Octave guard: a ~2x interval = one missed edge, split it (missed pulses
  used to audibly cut the deck to half speed); 8 consecutive doubles = a
  real tempo halving, adopted.
- Ghost-edge guard: an edge inside 60% of the locked period is
  bounce/crosstalk — ignored without corrupting the next measurement
  (extra edges used to wobble the playback rate).

**Tracker external sync WORKS** (bench + measured):
- Tempo-factor direction fixed (libxmp's factor is a time multiplier).
- Phase servo steers rows ONTO the clock pulses (compensating the ~0.5 s
  render-ring lead), measured: no cumulative drift (+0.5 ms/min), beat
  wobble ~±20 ms (engine tick granularity).
- IT-proof: beat phase counted in ticks (24/beat — correct for every
  speed), tempo ratio follows the module's LIVE bpm through IT tempo
  commands. Sequence-loop mode syncs too. Live shows "N bpm EXT" locked.

**Slicer .ot** (software-complete, hardware pass pending): 128 slices;
Octatrack .OT sidecars auto-apply on load (Mode Grid/Transient/OT);
GET /slicer/ot exports current slices as a real .ot; PUT /drop_ot + web
Upload passthrough; `⬇ .OT` link while the Slicer is active.

**Deck**: analysis on raw FatFS reads (~1 min/track, immune to concurrent
use, yields to playback and ring refills); smoothed ext-BPM display;
`ana N%` counts up.

**Fixtures/tools**: tools/make_clickmod.py (beat-click MOD),
tools/ot_tool.py (.ot maker/inspector), tools/analyze_drift.py.

## Known-remaining
- Deck +12 ms / tracker +15 ms constant lag vs pulse (trimmable).
- Tracker beat wobble floor = tick length (~20 ms at 125 bpm).
- .ot export byte-constants pending a diff against a real Octatrack file.
- Clock input conditioning still per-machine (unification queued).
