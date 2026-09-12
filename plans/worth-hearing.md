# Things worth hearing — audited 2026-09-11

This was a twenty-item "ear debt" ledger written 2026-07-30. Six weeks later most
of it was either overtaken by later work or quietly settled by months of actual
use, and carrying it made the few real items invisible.

**This is a music project, not a product.** The test is *does this change how the
instrument sounds or plays* — not *is it formally verified*. Something used
happily for weeks is tested. Nothing below is a blocker or a release gate.

The original list is in git history if it is ever wanted (`plans/ear-debt-20260730.md`
before this commit).

---

## Worth hearing, because it would change how it plays

1. **The LFO against a real clock — Synth and Keys.** The one thing that shipped
   to a public beta having never made a sound: the LFO landed 09-10 while the
   unit was offline. Sync does two things, and they fail differently — it sets
   the rate from the core clock, AND re-zeros the phase at every cycle boundary
   (`lfo.c`: `cyc = pulses / per; if (cyc != l->cyc) phase = 0`). So listen for
   both: wrong speed (obvious), and right speed but sitting off the beat or
   hiccuping at the cycle edge (the subtle one, and the reason the re-zero
   exists). Divisions are beats-per-cycle `16 8 4 2 1 0.5 0.25` = 4 bar … 1/16;
   shapes `sine tri saw sqr rnd`.
   Setup without menu diving — same preset keys on both machines:
   `lfs 1` sync, `lfv 0-6` division, `lfw 0-4` shape, `lfx 2` → pitch (pitch
   makes phase error far easier to hear than cutoff), `lfd` depth.
2. **Overdrive Bias sweep** (Keys, and Synth). The curve changed 07-26 — bias is
   no longer scaled by drive gain, which had made over half the knob a dead
   zone. Sweep bias at a few drive settings and listen for character across the
   whole range. This is a *sound* change, so it is worth an ear regardless of
   whether anything is "owed".
3. **Auto-tune on a real piano or EP.** Expect ~8–9 cents sharp from stretched
   partials — known and physical, not a bug. The question is only whether it
   lands close enough to finish with Fine.
4. **Tape `post` vs `pre` with Drive up.** `post` colours playback and records
   dry; the tape preamp always prints. Listen for the doubling the route fix
   removed, and that `off` does not dump level.

## Real bug, still open — promoted out of the old "results" section

**A multisample preset with reverb on drops its LAST zone when loaded.** The tank
allocates before the zone arena. Workaround: load with `rv 0`, then re-enable —
PAT_001 restored 5/5 that way. The fix belongs in `preset_load` ordering. This
one costs you actual work when it bites, which is why it outranks everything
above.

## Needs hands, and worth it for the same reason

**Knob-autosave across a POWER cycle.** Turn a knob, cut power, check it held.
The remote path passed 07-30; this is the other half, and a failure here loses
patches. The only item on this page where the downside is lost work rather than
a wrong noise.

---

## Closed by obsolescence (the code moved)

- **"Clock on CV8, drive quiet"** — the core clock (09-08) moved the clock to
  CV4 and put every machine on one detector. The K8→Drive un-wiring this was
  checking no longer describes the instrument.
- **Reverb mix-step pop test** — `tools/bench/mix_sweep.py` was written and run
  (`mix_sweep_20260731.log`), the mix slew and an always-on wet-step detector
  shipped at `0e09210`, and the pop hunt itself was retired 09-11: it lived on
  unit 1 (.227, still on July firmware), and the new units are the platform now.
  If the pop is ever heard again, read `rv.stp` from `/status?fx=1` — that is
  the whole diagnostic, and it rides along in every current build.
- **Reverb NaN flush** — `rv.nan` sat at 0 through a 3.5 h soak. Not a task; if
  the counter ever moves, that is the news.
- **3 h soak / autosave across reboot** — both done 07-30/31.

## Closed by use (months of playing is the test)

- One-shot declick, trimmed loop, loop-drag feel on Keys — all measured clean at
  the time and used constantly since.
- Drums OD level — the 6 dB trim shipped in July; Synth and Keys were confirmed
  by ear and Drums has been played since.
- Tape crop contents and the `CUT_` safety file — there are 142 takes in
  `usr/TAPE` on .85. If crop were losing content it would have surfaced.
- Overwrite-detector amber path, empty-slot cursor, wet/dry blanket item —
  edge cases nobody has tripped.

## Known-red, noted, not chased

- Waveform strip has no gain normalization (quiet samples draw flat).
- Sample browser lists the current folder as a row inside itself.
- CV6/CV7 clock-collision guards on other machines — superseded by the RESERVE
  idea: make the clock CV unselectable elsewhere. Partly there already, since
  `cvmtx.skip_src` keeps the clock channel from taking over a destination.
