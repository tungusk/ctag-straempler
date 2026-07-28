# keys-multisample-v1 (2026-07-28)

Full firmware snapshot at `keys-multisample-v1-20260728`. Continues
`flanger-fix-v1` (2026-07-27).

Serial-flash (recovery only — normal updates are `tools/ota.sh <IP>`):

    ./flash.sh [/dev/cu.usbserial-XXXX]

OTA layout (app @ 0x20000). Flashing an OLD single-app archive (e.g.
`bin/synth-v1`) reverts OTA and needs a re-migration.

## The headline: Keys is a multisampler you can actually reach

The 8-zone engine existed but had no UI at all — the only way to build a
multisample was to POST a `zones` array. It now has one, and three real bugs
found along the way are fixed. Everything below was measured through the bench
rig (`tools/bench/`), not judged by ear.

### Auto-tune obeyed a wrong filename by two octaves

`keys_autotune` lets a same-pitch-class name hint move the octave, because YIN
errs LOW on a strong sub-harmonic. The check had no bound and no direction, so it
equally obeyed a name that was simply wrong.

The card's `TSTC2` in fact holds a **C4** tone. Auto-tune heard 261.66 Hz — C4,
dead on — matched pitch class C against the name's "C2", and set the root two
octaves down. The zone played **2400 cents sharp with perfect intervals inside
it** and reported `fine = 0.00 cents`, which reads as a confident exact verdict.

Note the direction: moving DOWN from a correctly-heard pitch cannot be fixing a
sub-harmonic error. It can only be trusting the label over the recording, which
is the opposite of the rule it was written for. Now capped at one octave; beyond
that it reports `TUNE_CONFLICT` and the audio wins.

After the fix, swept across the whole keyboard and every zone seam:

    pitch error   mean -0.0 cents, worst +0.2, spread 0.4
    worst seam    0.2 cents        (audibility floor is ~10)
    zone select   9/9 correct
    level spread  0.2 dB

### A one-shot ended in a click

Running off the end zeroed the voice in a single sample, so the output jumped
from wherever the waveform was straight to zero — a click for any sample not
ending at a zero crossing, which is most of them. There is no audio past the end
to fade INTO, so the taper happens before it: the last ~5 ms ramp down.

    worst step at the end     before 0.034656   1.81x the material's own slew — CLICK
                              after  0.019156   1.00x — clean

### The default loop included trailing silence

`loop_end` was the whole file, so a looping sample wrapped from silence back to
full amplitude once per cycle. The crossfade cannot rescue that — what it blends
into IS the silence. Now trimmed back to the last sample above ~-60 dBFS.

    808CLAP  58.9% trailing silence removed     808HAT  0.3%
    808SN    33.4%                              808KICK none (ends on signal)

A held looping `808CLAP` now repeats at 194.4 ms — its trimmed length — confirmed
by autocorrelating the captured envelope to 0.1 ms. Untrimmed it was 472.4 ms,
i.e. more than half of every cycle was silence.

Only the END is trimmed. Choosing a loop START means guessing where the attack
ends, which is the player's judgement — hence the Live page control below.

### Also fixed

- The legacy flat `root/fn/lm/ls/le/lx` keys were applied to zone 0 AFTER the
  `zones` array. Round-tripping our own save is harmless, but `/remote/params` is
  a PARTIAL preset, so any caller sending zones alongside a leftover flat key
  silently lost zone 0's tuning and loop.
- `tune_src` and `tune_hint` are now PER ZONE. The instrument-level ones hold
  only the last LOAD's verdict, so in a multisample they say nothing about the
  other zones — and the verdict is exactly what you want when one zone of eight
  is out of tune.

## What is new on the panel

**Setup** gains four rows at the top. `Zone` picks which zone every per-zone row
acts on (sample, root, fine, auto-tune, all four loop rows) and retargets the
Live waveform with it. `Add Zone` appends via the browser and returns to Setup,
because you are usually adding several. `Clear Zones` drops the extras and KEEPS
zone 0 — a single click that leaves Keys silent with no undo does not belong on a
menu row, and Load Sample is already the full reset. Rows say what they will do:
`add >` or `full`, `drop 3 >` or `-`.

**Live** — the loop box is now editable. Browse to either edge and click in to
drag it against the waveform. One detent is one pixel (the step scales with
sample length; Setup's fixed 10 ms step is 0.13 px on a 22 s sample). Clicking
into an edge ARMS looping — browsing only shows you where the window sits, drawn
dim when off. The header becomes a readout while an edge is focused, with a
bracket on the side being moved: `loop [232-2834 ms`.

Verified: dragged both edges by hand, captured the held note, autocorrelated its
envelope — 2044.3 ms against the 2044.4 ms window set on screen.

## EAR DEBT — none of this has been heard

Every claim above is instrumental. Owed:

1. **The one-shot declick.** Load a drum one-shot with looping OFF and let it run
   out. It should end silently. The measurement says clean; ears decide whether
   5 ms is the right ramp or whether it now sounds soft.
2. **The trimmed loop.** Hold a looping `808CLAP` or `808SN`. It should be a
   continuous stutter with no gap. Also check the trim did not cut a real tail —
   58.9% off the clap is a lot, and -60 dBFS is a judgement call.
3. **Setting a loop by eye on Live.** Does one-detent-per-pixel feel right? Is
   arming-on-click surprising?
4. **A real multisample.** The zones tested were reference tones. Build one from
   an actual instrument across several octaves and listen at the seams — 0.2
   cents is inaudible on paper, but timbre steps between zones are not measured
   by anything here.
5. **Known, not fixed:** the waveform strip has no gain normalization, so a quiet
   sample (`808CYM`) draws as a nearly flat line and is hard to place a loop
   against by eye.

Carried forward unheard from `flanger-fix-v1`: overdrive Bias sweep, OD level on
Drums, Tape `post` vs `pre` with Drive up.
