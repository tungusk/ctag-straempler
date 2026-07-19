# taperack-v1 (2026-07-18)

Flashable snapshot. OTA-capable layout (ota_0/ota_1 + otadata). WiFi update:
`tools/ota.sh <IP>`. Serial recovery: `bin/taperack-v1/flash.sh [PORT]`.

## What's in this milestone (on top of fxnav-v1)
- **Shared FX rack is now clock-aware** (`components/fxrack/`): delay / flanger
  / tremolo gain a **Sync** toggle + **Div** (musical division); when a host
  feeds a BPM they lock to the grid. Keys/Synth feed no BPM -> unchanged.
- **Tape ported onto the shared FX rack**: Setup shows **FX1 / FX2 / FX3
  Reverb** slot rows opening the shared dynamic per-slot page (effect select +
  its params); process is one `fxrack_process_i32` with the grid BPM fed in, so
  dub delays lock. Rate effects default Sync on. (Trade-off: 2 generic slots +
  reverb, was all-5-at-once.)
- **Print-FX**: the record head taps the whole chain's output, so
  overdrive/flanger/tremolo/delay/reverb bake into the take (playback stays
  dry, no double-FX). Output **Level** is a master applied last — it no longer
  changes what's recorded (record gain is the hardware pot).
- **Play Mode**: loop / one-shot (stop at crop end).
- **Auto-save takes** (never silently lose a recording): a take persists to the
  card when you move on from it — a fresh take overwrites it, you load a sample,
  or you leave Tape. Non-racy on the single PSRAM buffer (deferred background
  write). Actively-cropped takes save the crop as **TCR_NNNN** (`crop:true`
  sidecar); raw takes save full as **TAP_NNNN**.
- **Re-record UX**: a stopped TR2 tap starts a fresh take (saving the old one);
  an overdub held ~0.7 s erases and rolls the fresh take on release. State-aware
  footer signposts the transport.

## NOT yet
- Auto-restore of the last take on returning to Tape (takes save to card but
  the buffer starts empty). Record-to-card mode for takes >30 s. Tape transport
  redesign. Deeper tracker internal-RAM fix. CV-matrix FX destinations /
  input-only FX machine / >2 slots.
