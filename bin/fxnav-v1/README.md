# fxnav-v1 (2026-07-18)

Flashable snapshot. OTA-capable layout (ota_0/ota_1 + otadata). WiFi update:
`tools/ota.sh <IP>`. Serial recovery: `bin/fxnav-v1/flash.sh [PORT]`.

## What's in this milestone (on top of midi-v1b)
- **Shared FX rack** (`components/fxrack/`): curated slots FX1/FX2 (generic) +
  FX3 (reverb) on Keys AND Synth. Per-slot Setup lines open a dynamic effect
  sub-page (effect select + its params); long-press returns to the line it
  opened. Float chain + single soft limiter (no inter-stage clipping).
- **Filter bricks**: LP/HP/BP filter + **base/width band** filter (FXK_BAND).
- **Keys + Synth Live encoder nav** (slicer idiom): turn cycles on-screen
  elements (sample/wave, dials, ADSR points), press clicks in to edit,
  long-press escapes. Synth adds an **oscillator preview** (VA morph / FM shape)
  and engine (VA/FM/WT) select on Live.
- **Tracker crash safety-net**: caps module size to the granted render-task
  stack -> refuses big modules with "too big (low RAM)" instead of panicking.
- Per-machine SD folders (usr/KEYS, usr/TAPE) + usr/MODS->usr/TRACKER migration.

## NOT yet
- Tape still on its old inlined FX chain (fxrack migration pending); Tape record
  is gate-only (on-device punch pending). Deeper tracker internal-RAM fix pending.
