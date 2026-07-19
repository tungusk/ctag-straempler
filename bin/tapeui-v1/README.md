# tapeui-v1 (2026-07-18)

Flashable snapshot. OTA-capable layout (ota_0/ota_1 + otadata). WiFi update:
`tools/ota.sh <IP>`. Serial recovery: `bin/tapeui-v1/flash.sh [PORT]`.

## What's in this milestone (on top of taperack-v1)
A ground-up polish pass on the **Tape** machine — its main-screen UI and the
recording workflow (still on the shared clock-aware FX rack from taperack-v1).

### Main-screen redesign
- **Big DejaVu24 filename title** that *is* the Load affordance: grey selection
  outline + bright-cyan text when selected, a green **check** once the take is
  saved to the card, and the name itself (`Blank Tape` / `REC-###` while
  unsaved / the saved `CUT_####` id). The `Tape` machine name + `state / pos /
  bpm` status share the top row, all on black.
- **Tall waveform colour-coded by transport state** — red recording, green
  playing, blue stopped — with a white centre **origin line**, a dim beat grid,
  and no box border.
- **Crop points selected in place** by scrolling (bold cyan edge, green when
  grabbed) with an `IN / OUT / beats` readout. Edit + FX sit in a **static
  button row**: Rev / Norm / Fade / Clear + **FX1 / FX2 / FX3** (each shows its
  effect name; press jumps into that FX slot).
- Encoder scrolls name → crop points → actions → FX; press = load / grab a crop
  point / fire an action / open an FX slot; long-press = Setup.

### Recording
- **Auto-restore**: the last take reloads when you return to Tape, so
  work-in-progress survives a machine switch (persisted in CONFIG `tapelast`).
- **Card-record mode** (Setup → Rec Dest → card): long takes stream straight to
  a WAV via the shared recording service, re-armed off the audio task — no 30 s
  cap.
- **Reliability**: takes auto-save the moment they **finalize** (stable buffer,
  no race), so re-record and the long-press erase-and-restart roll immediately
  with no stall. Take files are `CUT_NNNN` (manual Save Crop stays `TCR_NNNN`).
- **Rec Quant** (Setup → off / beat / bar): punch-out keeps recording to the
  next beat/bar so loops are a whole number of beats. TR1 cancels a pending
  beat-stop.
- Play Mode loop / one-shot; the "Armed: Release to Record!" cue on a long-press.

## NOT yet
- Auto-restore across a full reboot after an *unsaved* mid-record take (only
  finalized/loaded takes persist). Crop persistence across sessions (restore
  reloads the full take; you re-crop). Deeper tracker internal-RAM fix (bench).
