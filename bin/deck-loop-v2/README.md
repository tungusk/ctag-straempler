# deck-loop-v2 (2026-07-13, Arlo's ear-test session)

Firmware `machines-20260713m` — everything the ear test produced, on top of
import-v1.

DECK LOOP, rebuilt on a streamed mapping (reader owns playback->file):
- ladder 1/4 .. 256 beats (quarter-beat units); guard keeps a window >= one
  clock pulse. Verified: every rung locks E+0, S0.
- window moves/resizes are SCHEDULED at the reader's frontier — no starve, no
  hole, phase stays true (truncating the read-ahead was freezing the cursor and
  slipping the PLL).
- seam crossfade in the reader (blends the head against the tail continuing
  past the window) — Arlo: "sounds good".
- TRACK loop is a streamed beat-snapped window too: no seek, no mute, no PLL
  reset at EOF ("redetects tempo every cycle" — fixed; 3 wraps, E+/-0, S0).
- knob pickup after loop release (CV7 no longer slams playback speed).
- posbar: the box SHRINKS to the loop window, keeping the transport colour.
- Setup on the house grammar (click-toggles, [ value ] brackets); hint removed.
- CAPTURE: 4-beat loop on the click track, ~7 wraps — mean -4.01 ms, std
  0.48 ms, max beat-to-beat jump 1.6 ms.

REVERB becomes a per-pad SEND bus (drums): each pad has a Rev Send (default 0
— a kick stays dry), master row is a RETURN level, and a Send Tap toggle picks
pre- or post-filter. Arlo: reverb "sounds great".

CLOCK: octave preference — first lock lands in 80-140 BPM whatever the mult/div
(the fold is interpretation only; feeding it back into the gates deadlocked the
detector). Applies to deck + tracker.

TRACKER: loop window snaps to the beat grid while the clock drives; Live bar
darkens outside the loop window.

Flash: ./flash.sh (esptool with --flash_size detect; port /dev/cu.usbserial-3110)
