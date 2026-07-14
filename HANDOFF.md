# HANDOFF — cross-agent notes

Two Claude agents share this worktree. Convention going forward: before a
work session, `git log --oneline -5` and read this file; note your active
area below; stage files EXPLICITLY (no `git add -A` — it has already swept
another agent's in-progress files into unrelated commits twice).

## Active areas (update when you start/stop)

- **beatlisten agent** (this note's author): `components/machine/beatlisten.*`,
  `clock.{h,c}`, audio.c tap, deck/sampler3 clock-source rows, menu Listen/ClkOut
  rows, `/status` bl fields. Status: P1 committed, awaiting on-device soak
  (`/Users/arlo/claude09/bl_soak.py`). P2 (KICK/FLUX modes, looper/tracker/
  glitch/dualdeck AUDIO wiring, `POST /blisten`) deferred until the soak verdict.
- **doubledecker agent**: `machine_dualdeck/*`, `machine_deck/*`, `machine_tracker/*`,
  `machine/trig_gate.h`, `machine/cvsmooth.h`, `machine_looper/*`,
  `machine_drumsampler/*`, `machine_slicer/*`, `machine_granular/*`,
  `machine_glitch/*`, `machine_sampler3/*` (CV-spike hardening sweep).
  Status: **review findings 1,3-8,10 FIXED in `c50871c`** (proof build now excludes
  machine_dualdeck too — it never did, so the "core is machine-clean" guarantee had a
  hole). Working under Arlo's no-flash / no-serial rule while your soak runs, so all of
  it is build- and reasoning-verified only, NOT hardware-verified.

## ✅ DONE — trig acquisition (beatlisten agent, per your request below)

Implemented exactly as you proposed, in `audio.c` (my area): GPIO ANYEDGE ISRs on
TRIG0/TRIG1 stamp assert edges and validate the low width at **>= 200 µs**
(`TRIG_MIN_US`). The audio task publishes **`machine_io_t.trig_rising`** — one bit
per validated assert since the previous block, de-duped by assert timestamp, long
gates report once at the validation width, teleremote soft asserts merged in.
`trig_level` is unchanged (still the raw per-block sample). See the updated field
comment in `machine.h`.

**Your move, in your area:** consume `io->trig_rising` in `trig_gate.h` (press =
rising bit OR debounced level-assert; release logic unchanged) and take
`TG_DEBOUNCE` back to 1 — that kills both bugs: 1 ms gates always register, floating
glitches (<200 µs) never do. Build+proof pass with the acquisition side in; NOT
hardware-verified (no-flash rule while the soak runs — verify both after).

<details><summary>Original request (kept for context)</summary>

`audio.c:69` samples the trig pins once per block (0.726 ms) — a ~1 ms gate and a
floating-input glitch are indistinguishable at that rate; TG_DEBOUNCE 2 drops short
gates, 1 lets a floating TR2 toggle the loop. Fix: ISR-measured pulse width + sticky
validated-pulse flag.
</details>

## Review findings for the doubledecker agent (verified 2026-07-14, range 25170a1..ec1ce0f)

An independent review pass verified these against current file content.
Arlo wants them worked; ranked by severity.

### HIGH
1. **Default loop-length CV collides with the clock input.** `dualdeck.c:555`
   sets `clk_src = 7` (CV8) and `dualdeck.c:570` (ee177ff) sets
   `cv_llen[0] = cv_llen[1] = 7` — same channel. With a clock patched on CV8
   and a loop engaged, the loop-length knob logic reads the pulse train:
   pulses grab the ref (>DD_PICKUP), the lows between pulses remap the loop
   to 1/4 beat within ~5 ms of engaging → instant stutter ("loops jumping
   around"). Fix: skip loop-knob reads for any channel equal to
   `dd.clk_src & 7` (guard in the knob section), and/or default `cv_llen`
   off the clock channel; `dualdeck_preset_load` (:966) can reload the
   collision, so the guard is the robust half.

### MEDIUM
2. **trig_gate debounce imposes ~2 ms minimum trig width** (`trig_gate.h:44-59`,
   f557120). Logic is correct, but at one sample per 0.726 ms block,
   `TG_DEBOUNCE 2` means a standard ~1 ms eurorack trigger registers only
   ~35-40% of the time (tracker TR1 play / TR2 loop, deck, dualdeck).
   Either latch trig edges in the acquisition layer, or document ">=2 ms
   trigs". Also the "~3 ms" comment assumes 64-frame blocks — real blocks
   are 32 stereo frames (0.726 ms); same 2x error in the catch-up comment.
3. **Pending-remap phase bugs**: `dd_loop_remap` (`dualdeck.c:328`) and loop
   release (`:350-357`) compute phase/`ff` from the COMMITTED mapping while
   `rm_at` is pending — two knob moves within one ring-lead, or dropping a
   loop right after moving it, anchors the next window with the wrong phase
   (click/jump). Derive via the live mapping when `rm_at` is set.
4. **Catch-up slew engages even when nothing was borrowed** (`dualdeck.c:358`):
   every loop release degrades the live crossfader to a τ≈180 ms slew for
   ~0.3 s even under the new defaults where loops don't borrow the fader;
   window is a timer (ends with a residual fast step ~19% of full scale),
   not convergence. Gate it on actual borrowing; hold until |target−xf| small.
5. **Static state not reset on machine start**: `dualdeck.c` statics
   (`s_cv6_ref/s_cv7_ref/s_catch_*/s_mv*/s_len_idx` :313-317, `tg[2]/tc`
   :601-602, `s_focus_prev` :672, `s_tl_sig` :906) and deck.c's new medians
   survive machine switches. E.g. a sequencer gate held low during switch-away
   leaves `tg[].held > 0`; switch-back emits a phantom `TG_REL_SHORT` → deck
   arms/loop toggles by itself. Reset in start().

### LOW
6. `own_ch` misses cross-pairs (`dualdeck.c:684-685`): deck1 window vs deck2
   length on one channel defeats the focus rule — compare against both of the
   other deck's channels.
7. CV Map amber warning (`dualdeck_menu.c:721`) doesn't flag collisions with
   `dd.clk_src` (the HIGH above) or loop-vs-loop sharing.
8. Editing a loop's CV slot while engaged+live (`dualdeck_menu.c:748-750`)
   re-targets the knob without re-arming — set that deck's refs to −1 on any
   slot change.

### Pre-existing (not from this range, but reproduces Arlo's live symptoms)
9. **Resync gesture toggles the loop**: TR2 `TG_PRESS` fires the loop toggle
   (`dualdeck.c:618`) before the both-trig combo arms (0.35 s), so every
   resync flips the focused deck's loop on the way in; releasing TR2 last
   also toggles on the way out (debounce shifted `TG_REL_LONG` 1-2 blocks
   past the `latched` clear, `trig_gate.h:134`).
10. `tools/proof_build.sh` EXCLUDE list lacks `machine_dualdeck` (CLAUDE.md
    add-a-machine convention).

### Clean (verified, no action)
Stub UI commit (14abb40); preset save/load roundtrip of the matrix fields;
audio-path discipline across the whole range (no SD/alloc/log in process()).
