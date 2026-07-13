# folders-v1 (2026-07-13)

Firmware `machines-20260713h` — SD FOLDER ORGANIZATION on top of wav-v1.

- Layout: usr/ = the pool (uploads/imports/legacy flat cards), usr/REC =
  device takes, usr/LOOPS = looper saves, usr/MODS = tracker modules
  (pre-existing).
- ZERO MIGRATION: ids stay bare; sample_resolve searches every folder
  pool-first, so pre-folder cards work forever untouched. New files land
  sorted automatically.
- Sidecar trios (.JSN/.OT) live NEXT TO their audio (sample_resolve_aux);
  all machines + REST routes ported. Take/loop numbering = one readdir MAX
  pass across all folders (legacy files can't shadow foldered ones).
- Wire-verified: take recorded into usr/REC (WAV, d0), auto-pickup plays it,
  /files lists across folders, /files/raw fetches every request shape,
  legacy flat pool fully regression-checked.

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
