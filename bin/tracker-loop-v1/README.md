# tracker-loop-v1

Full firmware snapshot with the Tracker **sequence loop mode** (KO-II style),
hardware-tuned and Arlo-approved ("nailed it"). Branch `v09-machines`,
tag `tracker-loop-v1-20260709`.

Flash: `./flash.sh [PORT]` (default `/dev/cu.usbserial-3110`, `--flash_size detect`).

## Loop mode (Tracker)
A live step-region loop over the module's own sequencer — one stream, the loop
IS the audio (no mixing).

- **TR2** toggles loop on/off; **TR1** = play/stop.
- **knob7 / CV7** = loop length: `1 / 2 / 4 / 8 / 16 / 32` steps.
- **knob6 / CV6** = loop position (ranges the whole song, cross-pattern).
- **Row-exact** wrap via per-tick rendering; seam re-seats with `xmp_set_row`
  (preserves replay speed) — `set_position` only when crossing a pattern.
- **Cross-pattern**: an order-list step map lets the window span patterns and
  the position knob range the entire song.
- **Release** = *keeps-running* by default (jumps to where linear playback
  would be, via `xmp_seek_time`) or **freeze** (Setup → Loop Freeze, persisted):
  resume from the loop point.
- **Instant edits**: position and length re-anchor on the next row boundary;
  relocations only on row boundaries (no sub-row note bursts); CV deadband
  rejects ADC jitter.
- Render read-ahead trimmed (~2 s → ~0.5 s, 0.25 s while looping) so edits are
  heard promptly. New module load returns to normal play.

Builds on **tracker-v1** (de-hub + Live redesign + sample-name message panel).
Everything else on `v09-machines` (deck, looper, slicer, granular, glitch,
single Sampler, drums, freesound, teleremote) is unchanged.

## Known-not-done
- Tempo-sync status line still hidden (external clock detection unreliable).
- Loop position across a slow-tempo song can be coarse (whole song on one knob).
