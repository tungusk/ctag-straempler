# doubledecker-v1 — 2026-07-13 (evening ear-test pass)

Flashable snapshot of the full firmware at this milestone. `./flash.sh [PORT]`
(default `/dev/cu.usbserial-3110`).

## What landed

**DoubleDecker** (the machine formerly called DualDeck — renaming it orphaned the
old autosave, so its deck slots come up empty once):
- The deck's STREAMED loop, transplanted: wpos/rpos are playback counters, the
  reader owns the playback->file mapping, wraps its own reads at the window end and
  crossfades the seam. Loop length is bounded by the TRACK, not the ring
  (1/4 .. 256 beats). Window moves are SCHEDULED at the reader's frontier.
  The track loop streams too — no more EOF seek that reset the PLL every pass.
- CV MATRIX (Setup -> CV Map): Crossfade, Filter, and Loop Pos + Loop Len PER DECK.
  The loop's "borrow" is not special-cased — it is simply what happens when a loop
  shares a channel with the filter or fader. Give the loops their own CVs and both
  decks go live at once. Loop knobs otherwise follow FOCUS, and switching focus
  leaves the other deck's loop in state.
- Filter on CV6 (house convention), crossfader on CV7. Takeover auto-fade is
  opt-in and OFF by default. The fader is never dead: after a loop releases it, the
  ENGINE catches up to the knob (~0.6 s) rather than the knob waiting for pickup.
- Two layouts (Setup): V = two single decks stacked, black canvas, the deck's real
  transport bar (the box shrinks to the loop window), deck-number focus badge,
  hairline crossfader. H = the side-by-side panels.

**BOTH-TRIG RESYNC** (shared, trig_gate.h): hold TR1+TR2, and on RELEASE the beat
lands there. The track slides to the nearest beat on a capped rate bend (no seek)
AND the PLL's lock point moves with it, so it sticks. Live in Deck + DoubleDecker.
This is also the manual nudge, and it costs no knob.

**PLL: the lock lead was MIS-TUNED.** It sat at 13.1 ms and both machines landed
~4.3 ms early. Scarlett capture, 125 beats per run, put the zero-crossing at
6.8 ms:  13.1 -> -4.32 ms (std 2.53) | 10.6 -> -3.33 (0.70) | 6.0 -> +0.65 (0.22) |
**6.8 -> +0.43 ms (std 0.12, drift +0.01 ms/min)** — the tightest lock measured on
this instrument. Verified again after Arlo switched clock sources: +0.52 ms.

**The click was an ADC spike.** With hands off the panel, CV7 sat at ~1221 and threw
a SINGLE sample of 4. CV7 is the crossfader — it multiplies the whole mix — so one
outlier is a broadband click on both decks, on any track, masked by a low-pass.
Fixed by a shared median-of-5 (components/machine/cvsmooth.h) on all 8 channels.
NOT the loop seam, NOT starvation, NOT clipping — all three were measured and ruled
out first.

**Tracker**: RETRIG below one step (CV7's sub-step rungs: 1/2, 1/4, 1/8, 1/16 —
the score has no address finer than a row, so it stutters the RENDERED ROW instead,
and CV6 picks WHICH line). Loop ladder to 1024 steps. The DJ filter now lives on
CV6/CV7 outside loop mode (they previously scrolled a text panel that is now off by
default). The play bar is a solid light-green body with the loop window cut into it
dark. CV6 spans the whole song, and the length knob no longer drags the start.

**Core**: the Stub fallback finally has a UI — a fallback you cannot steer out of is
not a fallback (it had no pages at all, so a failed machine start left a dead screen
and System->Machine unreachable).

## Build caveat (2026-07-13)

These binaries were built from this commit with `components/menu/menu.c` reverted to
its 25170a1 state. A parallel session's in-progress "beatlisten" work was
accidentally swept into commit 4d7e3ea by a `git add -A components/menu`; that
menu.c calls `menuTFTPrintListen`/`menuTFTPrintClkOut`, whose definitions
(menutft.c/h, machine/beatlisten.c) are NOT yet committed. Once that session commits
its files, HEAD builds as-is again.
