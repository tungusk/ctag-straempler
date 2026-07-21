# tuner-autotune-v1 (2026-07-20)

Full firmware snapshot at `tuner-autotune-v1-20260720`. Covers TWO batches — the
07-20 drums/tape mega-session (which never got its own archive) and the
pitch-detection work built on top of it the same day.

Serial-flash (recovery only — normal updates are `tools/ota.sh <IP>`):

    ./flash.sh [/dev/cu.usbserial-XXXX]

OTA layout (app @ 0x20000). Flashing an OLD single-app archive (e.g.
`bin/synth-v1`) reverts OTA and needs a re-migration.

## What's in it

**Pitch detection (this batch, commits 72382fe + 3f9b064)**
- `components/util/pitch_detect.{c,h}` — shared monophonic YIN detector: coarse
  lag sweep on a 4x decimated window, native-rate refine + parabolic fit. Host
  tests: sub-cent 55 Hz..1.3 kHz on sines, 12-harmonic saws, plucks and noisy
  material; refuses noise and silence. Also parses a note name out of a sample
  id, reporting whether the match was isolated or buried in a word.
- **Keys auto-tune** — Setup rows Fine / Auto-Tune / Tune on Load. Detects the
  loaded sample's fundamental over the resident PSRAM buffer (~50 ms, no SD)
  and writes zone root + the new `fine` (cents; root alone only lands within
  half a semitone). The id's note name may fix the OCTAVE only, and only when
  it agrees with the audio on pitch class. Verified on hardware with four
  synthetic tones (220 Hz named A3 -> A3 0.0c; C4 audio named C2 -> C2;
  436.71 Hz named A4 -> A4 -13.0c; 220 Hz named F3 -> A3, audio wins).
- **Line-in tuner** — `components/machine/tuner.{c,h}`, beatlisten-shaped
  (audio-task tap + prio-4 task), OFF by default, System > Settings > Tuner
  page enables on entry / disables on exit. Rate-limited to ~4 readings/s
  (detection is ~35-40 ms/window) with a live-input level gate. `/status`
  "tun" object while on, `GET /tuner/enable?on=`.

**Drums / Tape (commits 15be163..5574da2, was to be `drumsfx-cvmtx-v1`)**
- Drums on the shared FX rack + per-pad wet/dry FX-bus routing, per-pad PITCH
  (±12 semitones) with a Pitch CV source/mode row (+/- and V/oct), scrolling
  Pads page, knob-edit autosave.
- Tape CV matrix on the new shared `cvmtx` widget + FX param destinations +
  Live Crop button; Synth/Keys matrices migrated onto the same widget.

## Verification status at archive time

Built + `proof_build.sh` pass; flashed and remote/screenshot-verified. NOT yet
fully ear-tested: tuner accuracy against a known steady tone, drums pitch/V-oct
tracking, wet/dry audibility, tails, knob-autosave across a power-cycle, and
the tape matrix by ear. Archived ahead of those on purpose — see git history if
anything needs chasing down.
