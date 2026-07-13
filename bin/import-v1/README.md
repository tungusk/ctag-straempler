# import-v1 (2026-07-13)

Firmware `machines-20260713k` — CONVERT-ON-IMPORT, the last format gap,
on top of dualdeck-v1. ANY audio file now works: native formats play
directly, everything else converts ONCE.

- components/util/sampimport: MP3 (helix, already aboard) + WAV PCM
  8/16/24-bit + 32-float + AIFF 16/24 at any rate 8-96 kHz -> native
  16/44.1 stereo .WAV via a streaming cubic resampler. Sources are
  REPLACED by their native twin (no collisions, no double listings).
- POST /import = pool-wide background scan (GET /import = progress);
  every /drop_sample upload sniffs its payload and kicks the importer —
  curl an MP3 at the module and it lands playable.
- Verified end-to-end: MP3 -> WAV -> deck locked E+1; 48 kHz WAV
  resampled -> deck locked E+0; clean pool scan (one harmless residual
  false positive on a clipped take — sources are never touched on
  decode failure).
- ALSO FIXED (ancient): /drop_sample wrote its alignment pad at the
  FRONT of uploads — harmless for RAW, corrupting for byte-exact
  containers. Pad appends now.
- MP3 sniff = ID3 tag or THREE chained valid frame headers.

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
