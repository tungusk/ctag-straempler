# Tape — streaming to/from the card (design, 2026-07-25)

Status: **DESIGN / not started.** Arlo-initiated after hitting the PSRAM wall
asking for "64 beats on one of these lazy 100bpm tracks". Goal: break Tape's
length ceiling without giving up the interactive editing that makes it Tape.

**Do not start implementing from this doc alone** — the overdub section below is
genuinely unsolved and wants a conversation first.

## The wall, measured

Measured on hardware 2026-07-25 via the new `/sysinfo` `psram`/`iram` report:

| | |
|---|---|
| PSRAM total | **4.00 MB** |
| Non-tape PSRAM in use (reverb tank, misc) | ~0.53 MB |
| Available to the tape | **~3.47 MB** |

**4 MB is a HARDWARE ceiling, not a config.** The ESP32 maps at most 4 MB of
external RAM into its data address space regardless of the chip fitted
(`CONFIG_SPIRAM_SIZE=-1`, auto). No sdkconfig change moves this.

So mono 16-bit at 44.1 kHz (88.2 KB/s) gives **~41 s maximum, forever**, and only
if nothing else wants PSRAM. Already banked:

- Banks cut 1 MiB → 128 KB (`TP_BLK_SHIFT` 19→16), recovering 0.38 MB that
  bank rounding was discarding (a 30 s tape is 2.52 MB of audio but rounded up
  to 3 whole 1 MiB banks). Length options are now 15/30/**40** s; 60 s was never
  reachable (5.05 MB of a 4.00 MB pool) and silently fail-softed.
- 40 s = 3.38 MB of banks against 3.47 MB available — **fits, with ~0.09 MB
  spare.** That covers 64 beats at 100 bpm (38.4 s), which was the ask.

**The catch:** `fxdelay` is a lazy ~690 KB slab. At 40 s there is no room for
it. It is "long tape OR a delay", not both. Reverb (~170 KB) is fine. This
trade-off is the real argument for streaming — not the seconds.

## What already exists (don't rebuild it)

- **Long capture is SOLVED today.** `Rec Dest = card` (`TPD_CARD`) streams a take
  straight to a WAV with no 30 s limit, bypassing the PSRAM tape entirely.
- **Long playback is NOT.** `tape_load` does `if (total > tp.cap) total = tp.cap`
  — a 3-minute card take is truncated to whatever the tape holds. You can record
  long; you cannot play, loop or edit long. **This asymmetry is the actual gap.**
- **The reader pattern is proven four times**: Deck, DoubleDecker, sampler3 and
  the slicer all run *unpinned reader task → PSRAM ring → `process()` touches
  only RAM*, with raw-FatFS reads under `sd_lock`.
- **Sampler3 already solved the closest problem**: windowed streaming where the
  stream is capped past the loop window so the ring always holds it, wraps are
  seam crossfades (~6 ms), full-file wraps prefetch the rewind under the playing
  head, and a starved-cursor self-heal (+150 ms sledgehammer) re-fires the seek
  protocol so it cannot wedge. **Read that code before writing any of this.**

## Recommended architecture: WINDOW IN RAM, TAIL ON CARD

The slicer's shape (attack heads resident, tails streamed), applied to Tape:

- **The crop window stays resident in PSRAM.** Everything interactive —
  cropping, reverse/normalize/fade, CV-modulated In/Out/Window, overdub, the
  Crop drop — operates on the resident window exactly as it does now. No
  behaviour changes, no new failure modes in the paths that already work.
- **Material outside the window is streamed** from the take's WAV on demand.
- **Moving the window** = a seek: prefetch the new region under the playing
  head, crossfade at the seam. Sampler3's protocol verbatim.

Why this shape and not "stream everything":

1. The CV matrix modulates In/Out/Window at **audio rate**. A ring cannot follow
   arbitrary jumps without dropping out. Keeping the window resident makes CV
   modulation free and exact rather than something to fight.
2. Overdub (below) only has to work against RAM.
3. It degrades gracefully: with a small window it is today's Tape; with a large
   file it is a long tape. One code path, not a mode switch.

## The hard part: OVERDUB on streamed material

Tape records *into* the loop while playing it. On a streamed file that becomes
read-modify-write against the card, under `sd_lock`, in real time — with seam
management where the written region meets the streamed one. **This is new
design, not a port.** Everything else in this plan is reusing proven code.

Options, roughly in order of increasing honesty:

- **(a) Overdub only within the resident window.** Cheapest and probably right:
  the window is where you are performing anyway. Punching in outside it either
  moves the window first (seek, brief mute) or is refused.
- **(b) Journal overdubs to a side file** and mix on playback. Avoids RMW, costs
  a second stream and a mixdown step to commit.
- **(c) Full RMW.** Correct in the general case, and the one most likely to
  produce the intermittent click family we have been chasing all year. Not
  recommended without a bench rig.

Recommend **(a)** for v1 and revisit only if it actually bites in use.

## Staging

1. **Streaming PLAYBACK of long takes (read-only).** Port the deck reader; drop
   the `tape_load` truncation; window stays resident. Biggest win, lowest risk —
   long loops, cropping, FX, the lot. Overdub refused outside the window.
2. **Unify card-record with it.** A card take becomes directly loopable/croppable
   instead of a file you have to reload. Closes the record-long/play-short
   asymmetry.
3. **Window seek polish.** Prefetch + seam crossfade + starved-cursor self-heal,
   lifted from sampler3.
4. **Overdub beyond the window** — only if wanted, per the options above.

## 22 kHz option — orthogonal, and cheaper

Arlo likes this independently. Store the tape at 22.05 kHz: ~78 s in the same
RAM (130 beats at 100 bpm), with a lo-fi character that suits a tape machine.

Mechanically easy — the engine already carries a `double pos` cursor, so
playback advances 0.5 per output sample with interpolation, and recording
decimates on the way in (2-tap average as a crude anti-alias).

**The risk is not the DSP, it is `TP_RATE`.** 44100 is baked into beat-frame
math, crop readouts, fade lengths and the peaks scan. It becomes `tp_rate()` and
touches ~15 sites — exactly the hand-parallel-constant shape that produced the
`SAMPLE_DIR_N` out-of-bounds crash twice (`SF_DIRS`, then
`sample_list_recent.c`). If this is done, do it with a `_Static_assert`-style
sweep: grep every literal 44100 and every `TP_RATE` in one pass, and change them
together or not at all.

With the window-in-RAM architecture, 22 kHz stops being a workaround for the
ceiling and becomes an honest tone choice — which is the better reason to have
it.

## Open questions for the planning session

- Is unlimited length actually wanted, or is 40 s + a delay the real requirement?
  (If the latter, this whole plan is unnecessary — say so early.)
- Does overdub outside the resident window matter in practice?
- Should a streamed Tape auto-commit edits to a new take (the Crop drop already
  establishes the "write a new file, adopt it" idiom), or edit in place?
- 22 kHz: per-take property stamped in the sidecar, or a global Tape setting?
