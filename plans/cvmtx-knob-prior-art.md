# Extend cvmtx to absorb the machines' knob prior art

## Context

The machine-side CV rework converts the hand-rolled machines onto the shared
`cvmtx` widget. Slicer went first (`74710cf`). Planning Deck, I characterised
its pickup machinery as code the conversion would "delete" — and Arlo pushed
back: that intricate work is the feel of these machines, so if the matrix can't
express it, **the matrix should grow to fit the prior art rather than the prior
art being thrown away.**

He is right, and checking proved it. Two further pieces of context that reframe
the whole rework:

**These machines were built on the first prototype, which had unusable knobs.**
Only K6 and K7 worked (K5 dead, K8 half-range — `project-cv-knob-hardware`).
Every context-switch conditional in the codebase is *multiplexing*: fitting four
or more jobs onto two knobs. `synth.c:102` says it outright — *"Built units have
all four; this dev unit's K5/K8 are weak, which is what the takeover is for."*
Development is now on the **second prototype**, which has four working knobs, so
the multiplexing can be undone and jobs can live simultaneously.

**But multiplexing and pickup are different things.** Undoing the first must not
destroy the second. Pickup exists because a physical knob's position may not
match the parameter's value — that is true on any hardware, and it is what stops
a loop engage from flinging the window to wherever the knob happens to sit.

So: extend the widget first, then every conversion is lossless.

---

## The three gaps, with evidence

### 1. Only one takeover behaviour — grab. Tracker needs catch.

**Deck** compares the knob to *where the knob was when armed* (`deck.c:741`):
```c
if (c7 - s_cv7_ref > DK_PICKUP || s_cv7_ref - c7 > DK_PICKUP) s_cv7_ref = -2;
```
`cvmtx` does exactly the same (`cvmtx.c:64`):
```c
if (!m->live[d] && fabsf(k - m->capt[d]) > CVM_TAKEOVER) m->live[d] = true;
```
Thresholds match to a rounding error — `DK_PICKUP` 120/4096 = **0.029**,
`CVM_TAKEOVER` = **0.030**. Deck converts losslessly today.

**Tracker does not.** It compares the knob to *the current parameter value*
(`tracker.c:776-777`):
```c
if (pk6 != -2) { int d = c6 - trk.filt_cv; if (d <= TRK_PASSTOL) pk6 = -2; }
```
That is pass-through / **catch**: the knob is inert until it crosses the value,
so there is no jump. Converting Tracker with cvmtx as-is would slam the filter
to the knob position on any movement — exactly what its own comment says the
pickup exists to prevent.

### 2. Rearm is all-or-nothing

`m->rearm` is a single bool (`cvmtx.c:53-54`). Deck re-arms only its loop knobs
on disengage (`deck.c:804`); DoubleDecker has a per-deck
`dualdeck_rearm_loop_knobs(int deck)`. Calling `cvmtx_rearm()` would also rearm
a filter that has nothing to do with looping.

### 3. No conditional liveness

Deck's loop knobs are live only while a loop is engaged. DoubleDecker's
`dd_addressed()` decides which decks' knobs are live based on whether their
channels collide — its comment calls that *"the whole point of the matrix"*. A
host can ignore the value, but then `capt`/`live` drift underneath it and the
knob is wrong when it becomes relevant again.

**What correctly stays host-side** and should not move into the widget: Deck's
quantized ladder (`dk_loop_q[]`), its 40-count boundary hysteresis and 3-block
confirm. Those are Deck's judgement about its own parameter, not generic knob
behaviour.

---

## The design

Three new per-destination fields and four small functions in
`components/menu/include/cvmtx.h` + `components/menu/cvmtx.c`. All of the change
lands inside `cvmtx_track()`, which is ~25 lines.

```c
#define CVM_TK_GRAB  0   // live once the knob MOVES past the threshold (today)
#define CVM_TK_CATCH 1   // live once the knob CROSSES the current value
#define CVM_CATCH_TOL 0.022f   // Tracker's TRK_PASSTOL 90/4095, lifted verbatim

uint8_t  tk[CVMTX_MAX];       // per-dest takeover behaviour, default GRAB
float    base01[CVMTX_MAX];   // host's current value 0..1 — CATCH compares to this
uint16_t hold;                // bit per dest: cannot take over while set
uint16_t rearm;               // WAS bool — now a bit per dest
```

```c
void cvmtx_set_takeover(cvmtx_t *m, int d, int tk_mode);  // init-time
void cvmtx_set_base(cvmtx_t *m, int d, float v01);        // host's value, for CATCH
void cvmtx_rearm_dest(cvmtx_t *m, int d);
void cvmtx_hold(cvmtx_t *m, int d, bool held);
```

In `cvmtx_track()`, the takeover test becomes:

```c
if (rearm_d || m->src[d] == m->skip_src || ((m->hold >> d) & 1)) {
    m->capt[d] = k; m->live[d] = false; m->last01[d] = -1.0f;
    continue;                       // held: capture follows the knob
}
if (!m->live[d]) {
    m->live[d] = (m->tk[d] == CVM_TK_CATCH)
        ? (fabsf(k - m->base01[d]) <= CVM_CATCH_TOL)
        : (fabsf(k - m->capt[d])   >  CVM_TAKEOVER);
}
if (m->live[d]) m->base01[d] = k;   // so a later rearm catches against the truth
```

**`hold` subsumes most of the rearm need.** While held, the capture follows the
knob every block; when the host clears the bit the destination is armed exactly
where the knob sits, dead until moved. That *is* Deck's re-arm-on-disengage and
DoubleDecker's `dd_addressed()`, for free. `cvmtx_rearm_dest()` remains for
one-shot cases (a preset load touching a single destination).

**Nothing is persisted.** `tk[]` is an init-time host property like `labels`;
`hold`/`rearm`/`base01` are runtime. So `cvmtx_save`/`cvmtx_load` and the
`mxs`/`mxa`/`mxm` preset format are untouched — **no migration, no risk to
existing patches.**

---

## Backwards compatibility — the hard requirement

Tape, Synth and Keys are shipped, in daily use, and must behave **identically**.
They set none of the new fields, so the defaults have to reproduce today exactly:

- `cvmtx_init` memsets, so `tk[] = CVM_TK_GRAB` (0) and `hold = 0` — correct.
- `cvmtx_reset_defaults` currently sets `m->rearm = true`; as a bitmask that
  becomes `m->rearm = 0xFFFF`. **This is the one line where a mistake would
  silently change three working machines.**
- The `base01` write only happens on live destinations and is only *read* in
  CATCH mode, so GRAB hosts are unaffected.

---

## Step 0 — the bail marker, before touching anything

`cvmtx` is shared by three shipped, in-daily-use machines. If the takeover
rewrite is subtly wrong, Tape, Synth and Keys all change feel at once and it may
not be obvious which change did it. So tag first, following the existing
convention (`pre-core-clock-20260908`, `pre-cvmtx-abs-20260908`,
`pre-redraw-speed-20260909`):

```
git tag pre-cvmtx-takeover-20260912 413736f     # current tip
```

Local tag only — no push needed for a bail marker. Recovery is
`git reset --hard pre-cvmtx-takeover-20260912`, rebuild, OTA.

Also worth noting the flashed slot: `.85` currently runs `413736f` on ota_0, so
a power-cycle rollback reaches the other slot if a flash goes bad.

---

## Files

- `components/menu/include/cvmtx.h` — the four prototypes, the three fields, the
  `rearm` type change, the two new constants, and a comment block explaining
  grab vs catch (this header is the machines' reference; Slicer's bug came from
  reading the header and not a caller).
- `components/menu/cvmtx.c` — `cvmtx_track()` takeover block, the four new
  functions, `cvmtx_reset_defaults()` rearm line, `cvmtx_init()` unaffected.

No machine changes in this pass.

---

## Verification

1. Build clean with the IDF 4.3 invocation.
2. **Regression is the whole point of this pass.** Flash and confirm Tape,
   Synth and Keys are unchanged: on each, the panel knobs are still dead until
   moved, still take over at the same point, and `Reset` still restores the
   defaults. Synth's K5/K8 weakness on the old unit is why takeover exists —
   on `.85` all four should grab cleanly.
3. Save and reload a preset on Tape; confirm `mxs`/`mxa`/`mxm` round-trip
   unchanged (the format did not move, so this is a sanity check, not a
   migration test).
4. No new behaviour is reachable yet — no machine sets `tk`/`hold`. That is the
   point: this pass should be invisible.

Then the conversions follow, in this order, each now lossless:
**Deck** (grab, fits today) → **Tracker** (needs CATCH + hold) →
**DoubleDecker** (needs hold per deck for `dd_addressed`) → Sampler3 → Drums.
