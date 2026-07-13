# streaming-slicer-v1 (2026-07-13)

Firmware `machines-20260713i` — the STREAMING SLICER, on top of folders-v1
and wav-v1. The slicer's length ceiling is gone.

- Per-slice 80 ms attack heads in one 1.8 MB PSRAM slab (playback order;
  reverse heads pre-flipped at build). Trigger latency = RAM.
- Playing slice's tail streamed by a reader task into a 2 s ring (deck
  discipline: request flags, sd_lock per burst, small first chunk => ring
  live in ~15 ms, inside the 2x-pitch budget).
- Any-length pool sample, any container (RAW/WAV/AIFF via sampfile). Grid /
  transient / .ot slicing unchanged; transient detection env now PSRAM
  (cap ~10 min). Loads are async (one-pass peaks+env scan, ~1.5 min per
  8 min of audio — screen shows loading).
- Wire-verified: 82 MB / 7.8-min file, 32 slices, 20 rapid advances across
  the file = zero starves. /status v1: "P c<cur> n<slices> S<starve> g<gen>
  a<ring_avail>".
- Load Mono setting removed (the ceiling it dodged no longer exists).

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
