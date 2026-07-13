# looper-v3 (2026-07-13)

Firmware `machines-20260713d` — the looper HOUSE-STYLE REFRESH, on top of
clock-unify-v1 (all machines on the shared clockin_t front-end).

- Live lanes: black-canvas playbar with a bold grey waveform thumbnail
  (built from the PSRAM loop on record-finish), state-colored playhead as
  the only bright element, slice-based playhead redraws (no bar strobing).
- Setup: house row grammar — Sync/Monitor/BP Filter flip on click (no edit
  mode), Clock Src/Bars click-to-edit with an explicit [ value ] bracket,
  per-row repaints.
- Hint line carries the full TR grammar (press/TR1:rec TR2:play/stop).

NOTE: shipped pre-verdict at Arlo's request ("i'll review it later").

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
