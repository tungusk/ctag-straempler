# drums-v2 — the drum machine becomes an instrument

Full-firmware snapshot at the `drums-v2-20260713` tag (2026-07-13), built from
`version.txt = machines-20260713 (drums-v2)` — the About page should say so. Flash
it with `./flash.sh [port]`.

Drums went from "eight CV-triggered pads" to something you can play with two hands:
four pads, each able to carry a second sound that chokes the first, two knobs that
perform whatever the encoder is pointing at, and a master DJ filter. The sampler and
the deck picked up a shared browser and a legibility fix along the way.

## What's in it (over sampler3-v2)

**Drums: performable knobs.** knob6 and knob7 drive the *selected* pad, and both are
NEUTRAL at 12 o'clock. knob6: noon = unity, counter-clockwise fades out, clockwise
drives up to 4x through a cubic soft clipper (tangent to full scale, so a driven pad
leans into the ceiling instead of slamming into the int16 clamp). knob7:
counter-clockwise chokes the decay (1.5 s down to 20 ms); clockwise drives one of four
per-pad targets — **retrig** (loop the head, shorter the further you turn: a roll that
tightens into a buzz), attack, start offset, or none. A knob only takes over a value
once it actually MOVES, so selecting a pad can't slam its settings to wherever a knob
happens to sit — and at centre a pad is simply the untouched sample, so a CV that
never swings below half leaves it alone rather than gating it off.

**Drums: retrig with a repeat cap.** `sample` runs the loop out over the sample's
natural length, a count (2..16) makes it a fill, `INF` holds the stutter until the pad
is hit again. Every repeat gets its own ~0.7 ms ramp — the loop seam lands on an
arbitrary sample value and would tick otherwise.

**Drums: per-pad A/B choke layers** — the replacement for 8-pad mode, which is gone.
A pad can hold a second sample on its own CV trigger, sharing ONE voice, so either hit
interrupts the other: the open/closed hi-hat. Four cells, eight sounds, eight triggers.
Per layer: the buffer, its trigger, its own Schmitt state. Per pad: everything
performable, because a knob that meant different things depending on which layer last
fired would be unplayable. Buffers are allocated lazily now, so an empty kit costs zero
PSRAM (it used to claim 1.41 MB for pads that stay empty) and a failed alloc fails soft.

**Drums: master filter.** A fifth thing the encoder selects — a box in the middle of the
menu bar. knob6 is the deck's DJ sweep (centre bypass, left = LP down, right = HP up);
knob7 is resonance, which the deck doesn't have. That resonance knob is why this filter
can't just copy the deck: variable damping plus a high coefficient self-oscillates, so
the ceiling is lower, the damping floor rises with the cutoff, and there's a NaN guard
(a NaN in an SVF is permanent silence and would read as dead hardware).

**Drums: a Live grid you can trace.** A layered pad draws as two self-contained
half-cells — each with its own hit dot, name, trigger tag and rectified half-wave, the
two waves converging on the cell's midline with the details outside them. The selection
box is the HALF, not the cell. The encoder traces the grid rather than walking the
array: `1A > filter > 2A > 2B > 4A > 4B > 3B > 3A > 1B`, every step landing on a half
that physically touches the last.

**Shared, not copied: `components/util/svf.{h,c}`.** The Chamberlin SVF had been
hand-written three times (deck, looper engine, looper bounce). It is now one kernel;
deck and looper are ported onto it, and the port was proved bit-identical on the host
(2000 blocks x 64 frames of random signal and coefficients, compared bitwise) rather
than assumed. Note the deck's `1.2f` clamp is not a tidy-up candidate — at 12 kHz the
unclamped coefficient is ~1.51, so that clamp IS the top of its sweep; it survives as a
parameter.

**Sampler + deck: one browser.** `sample_list_recent()` — 512 entries in PSRAM, sorted
NEWEST FIRST, evicting the oldest rather than dropping the newest when full. The deck's
old browser was 224 names alphabetical, so a fresh upload sorted into the middle and
fell off the cap entirely once the library grew.

**Sampler + deck: grey waveform, white playhead.** A dense track filled the transport bar
with white columns and swallowed the white playhead whole. The waveform is grey now and
the playhead is the only bright thing in the bar.

## Known-remaining

- Drums has no PITCH: the mixer reads with an integer cursor, so pitch means fractional
  reads + interpolation — a real change to the audio path, deliberately not slipped in.
- The cross-layer choke fade (`DR_CHOKE_STEP`, ~3 ms) is a by-ear constant.
- sampler2 removal still pending; `/files/raw` Not-found bug still open.

## Flash

```
./flash.sh [/dev/cu.usbserial-XXXX]
```
