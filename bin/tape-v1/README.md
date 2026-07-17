# tape-v1 — 2026-07-17

The "Tape" machine: single-track tape recorder/editor (big-waveform crop UI,
IN-anchored beat grid, FX chain in the record path, cut/copy/paste, save to
pool takes) + the full machine-docs set in docs/machines/. Bank-list PSRAM
storage (1 MiB blocks; >2.1 MB single allocs are refused on this board).
`./flash.sh [PORT]` = serial recovery, OTA layout.
