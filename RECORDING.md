# Recording

## Setup

1. Plug audio into the line input (3.5mm or Eurorack level — check hardware).
2. Navigate to **FX** → **Ext In** and enable it (`Active: ON`). Set volume/pan as needed.
3. Navigate to **FX** → **Recording**.
4. You'll see `TRIG0` and `TRIG1`. Select the trigger input you want to use for recording.
5. Press the encoder to select it, then scroll to change its mode:
   - `VOICE` (default) — gate triggers sample playback, no recording
   - `RECORD` — gate controls recording only; voice playback is suspended while recording
   - `TRANSPORT` — gate controls recording AND voice playback at the same time

## Record

- Send a gate HIGH on the configured TRIG input → recording starts (REC indicator lights on screen)
- Gate LOW → recording stops and file is saved

## Output files

Recordings land on the SD card at `/sdcard/usr/REC_0001.RAW`, `REC_0002.RAW`, etc. Each recording also gets a `.JSN` sidecar at the same path. The `.RAW` format is 16-bit stereo interleaved at 44100 Hz (same format as all sample files on the card).

## Playing back a recording

The recorded files appear alongside your other samples in the file browser. Navigate to a voice's file selector and you'll find the `REC_xxxx` files listed there.

## Quick test (no gate source)

If you don't have a gate signal handy you can't test start/stop from TRIG — that requires a hardware gate. You'd need to check whether there's a manual record toggle in the menu or wire something to the TRIG jack.
