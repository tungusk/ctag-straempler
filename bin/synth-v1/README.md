# synth-v1 — 2026-07-15

Full firmware snapshot at the Synth milestone (marathon eve-test session).
Flashable known-good state: `./flash.sh [PORT]` (default port
`/dev/cu.usbserial-3110`).

## What's in this build (highlights beyond machines-20260713n)

The **Synth machine** grew from a basic subtractive voice into a full
performable instrument — the operability blueprint for the future tonal
instrument sampler:

- **4-macro-knob layout** (developing for built units w/ 4 working knobs, ch5–8):
  K5 = engine timbre (VA shape / FM index / WT fold), K6 = cutoff, K7 = resonance,
  K8 = env→cut. Each knob uses **takeover** (Setup/default holds until the knob is
  moved) so the dev unit's weak K5/K8 don't fight the sound.
- **Live settings dashboard** — compact header (engine + note, green) + four
  labeled knob **dials** (dim until a knob takes over) + an **ADSR curve**;
  redraws only on change (no free-running meter).
- **CV matrix** — 8 destinations (Cutoff/Reso/Timbre/Env>Cut/LFO Rate/LFO Dep/
  Level/Pitch), each a source (CV1..8 / off) + bipolar amount, median-conditioned,
  ch1/2 floor-rescaled, adds on top of the base. Reachable from **Setup → CV Matrix**.
  Persisted in the patch.
- Cutoff full range 10 Hz..6 kHz (closes down); WT wavefold; FM index live on K5.

Other machines/fixes since the last archive (this session):
- **Radio**: press = play/stop, redraw throttle, SPAZ preset, `[ ]` markers, bigger
  station name, and a **station-change wedge fixed** (two-task race → generation guard).
- **Editor** idle-flash fixed; **System machine picker scrolls** (roster overflowed).
- On-device **Settings → Bounce** toggle; web perform-slider tweaks.

## Recovery
This is also the safety-net image for the upcoming OTA partition migration — if
an OTA layout change bricks the device, `./flash.sh` restores this known-good
single-app-partition firmware over serial.

Matching git tag: `synth-v1-20260715`. Commit at archive time: `ecbc992`.
