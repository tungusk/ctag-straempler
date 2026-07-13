# wav-v1 (2026-07-13)

Firmware `machines-20260713g` — NATIVE .WAV/.AIFF across the pool, and
recordings/loop saves written as .WAV. The RAW format becomes invisible
plumbing.

- components/util/sampfile: one format seam (probe by MAGIC, not extension;
  RAW / WAV PCM-16/44.1k mono+stereo / AIFF same in big-endian). Rejections
  carry a reason ("WAV: not 44.1kHz") instead of silence. All conversion in
  reader tasks — audio tasks still only see native frames.
- Extension-less ids everywhere; sample_resolve maps id -> container (.RAW
  wins ties). Lists/browsers/web/DELETE handle all containers.
- Streaming verified on hardware: deck lock on a WAV = RAW-identical
  (mean −0.54 ms, std 8.09 over 483 beats vs baseline −4.41/7.93); exact
  analysis on WAV: 120.0000 conf 1.00; mono WAV + AIFF verified; 48 kHz
  cleanly rejected.
- Recordings: REC_%04d.WAV with patched header (power-cut takes self-heal);
  looper saves LOOP_%04d.WAV. Take numbering = one readdir pass (the
  per-index stat scan starved the capture queue — 1804 dropped chunks,
  bench-caught, fixed same day: d0).
- MP3 stays import-only by design (no sample-accurate seeking).

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
