# CV matrix stage 3: knob jobs become matrix entries (ABSOLUTE mode)

## Context

The Remote tab's CV MATRIX card shows each machine's fixed knob jobs as read-only
chips ("K6 cutoff") beside the assignable matrix cells. Arlo's question from the
09-08 rework: "can assigned CVs be overridden?" — today only the picks (pitch, gate,
clock) can; the K5–K8 jobs are hard-wired in each host's `process()`:

- **Synth** (`components/machine_synth/synth.c` ~108–140): `kn[4]` from cvm[4..7],
  takeover (`knob_capt/knob_live`, 0.03 threshold, re-armed on engine change via
  `knob_engine`), K5 engine-aware timbre / K6 cutoff `10·600^k` / K7 reso / K8
  env>cut, dirty-flag hysteresis for autosave.
- **Keys** (`components/machine_instsampler/instsampler.c` ~425–447): same
  machinery, K5 start offset / K6 cutoff / K7 reso / K8 env>cut, re-arm via `knob_ctx`.
- **Tape** (`components/machine_tape/tape.c` ~330–370): K5 window move
  (`(k-0.5)*2`, performance-only, unsaved) / K6 cutoff `30·200^k` / K7 reso; K8
  deliberately unwired (Drive is Setup-only, 07-25); the clocked channel is
  excluded from takeover + dirty via `clk_kn` (now from `clock_core_src()`).

The assignable matrix is the shared widget `components/menu/cvmtx.{h,c}`: N
host-labelled destinations, each `src` (-1/0..7) + bipolar `amt`, OFFSET semantics
(`cvmtx_val()` = amt × conditioned CV, host adds it to the base). Persistence
`mxs`/`mxa`/`mxl`; device page label|[src]|[amt] with press cycling nav>src>amt;
web cells in `index.html` (`mxCell`/`rpMatrix`/`rpApply`/`mxReset`, ~1105–1190).

**Goal:** the knob jobs become ordinary matrix entries with a per-entry mode,
**OFFSET** (today's behaviour) or **ABSOLUTE** (takeover: once the source moves
past the threshold it *replaces* the base value, exactly what the knob code does
now). Defaults reproduce the current wiring, so nothing changes until a user
re-aims a knob, clears it, or adds a second absolute row. Synth, Keys, Tape.
Decided earlier: TR gestures stay picks; Sampler3's bespoke matrix and DoubleDecker's
CV map are out of scope.

## Design

### Widget: `components/menu/cvmtx.{h,c}`

State added to `cvmtx_t`:
```c
uint8_t mode[CVMTX_MAX];     // CVM_OFFSET (0) / CVM_ABS (1)
const int8_t *def_src;       // host defaults per dest (-1 none); a defaulted dest is ABS
float   capt[CVMTX_MAX];     // ABS takeover: captured position / live flag / last
bool    live[CVMTX_MAX];     //   committed position (dirty hysteresis)
float   last01[CVMTX_MAX];
bool    rearm;               // next track(): recapture, all ABS entries go un-live
int8_t  skip_src;            // -1, or a CV channel that must never take over (the clock)
uint16_t nodirty;            // bit per dest: ABS moves there don't flag autosave
```
API:
- `cvmtx_init(m, labels, n, def_src)` — signature grows by the defaults; applies
  them (`cvmtx_reset_defaults(m)`: all entries off, then `src=def, mode=ABS` where
  `def>=0`). `skip_src=-1`, `rearm=true`.
- `cvmtx_track(m, cvm)` — besides the ch1/2 floors, runs the ABS takeover for every
  ABS entry with a source: `k = cvmtx_cv01(src)`; on `rearm` capture + un-live;
  `!live && |k-capt|>0.03 → live`; an entry on `skip_src` is held (capt=k,
  live=false); dirty hysteresis (`|k-last01|>0.03 → machine_state_dirty()` unless
  `nodirty` bit) — `machine.h` is reachable (menu REQUIRES machine).
- `bool cvmtx_abs(const cvmtx_t*, int d, float *k01)` — true when dest d is ABS,
  sourced, live (and not skipped); gives the position 0..1. Hosts apply their own
  mapping (the same formulas the knob code uses today).
- `cvmtx_val()` returns 0 for ABS entries (offset path untouched).
- `cvmtx_rearm(m)` replaces the hosts' `knob_ctx = -1` / `knob_engine` re-arms.
- Persistence: save adds `"mxm"` (modes) and `"mxd"` (defaults, for the web Reset).
  Load: honours `mxm` when present; **migration** when absent (every existing
  autosave/patch, incl. the legacy `msrc/mamt` shape): for each dest with a default
  and `src[d] < 0` → `src=def, mode=ABS`. A dest already carrying an OFFSET
  assignment keeps it and gets no absolute knob — which is precisely the fix for
  the 07-28 "Cutoff ← CV6 at full amount while K6 is cutoff" double-drive.
- Device page: the **amount field wraps into "knob"** — FWD past +100 % sets ABS
  (amt column shows `knob`), BWD from `knob` returns to +100 % OFFSET. Press cycle
  nav>src>amt unchanged. (Recommended over a fourth field; say so if you'd rather
  have an explicit mode field.)

### Hosts (pattern; Synth is representative)

1. Defaults table next to the labels: Synth `{Timbre←CV5, Cutoff←CV6, Reso←CV7,
   Env>Cut←CV8}`; Keys `{Start←CV5, Cutoff←CV6, Reso←CV7, Env>Cut←CV8}`; Tape
   `{Window←CV5, Cutoff←CV6, Reso←CV7}` (no K8 — Drive stays Setup-only).
2. Delete `knob_capt/knob_live/knob_ctx|knob_engine` fields and the whole knob block
   (capture, takeover, apply, dirty). Re-arm points (`knob_ctx = -1`,
   `knob_engine = -1` at start/preset load/patch load; Synth's engine-change
   recapture) → `cvmtx_rearm()`; Synth keeps a static `last_engine` to call it.
3. After `cvmtx_track()`, apply ABS entries with the existing formulas:
   `if (cvmtx_abs(&sy.mtx, SYM_CUTOFF, &k)) sy.cutoff_base = 10·600^k;` reso `k`,
   env>cut `k`, timbre engine-aware (fm_index `k·8` / fold `k` / shape `k`). Keys:
   start_frac `k`, cutoff, reso, env>cut. Tape: `win_move = (k-0.5)·2`, `cutoff =
   30·200^k`, `res01 = k` — and **Tape needs a new destination `TPM_RES` ("Reso")
   appended AFTER `TPM_RVMX`** so old `mxs` indices stay valid; `tp_rk`/offset
   reads unchanged. Tape sets `tp.mtx.skip_src` from `clock_core_src()` (0..7 else
   -1) each block and `nodirty = 1<<TPM_WIN` (window move stays performance-only).
   Dests without a natural absolute meaning (Level, Pitch, LFO rate/depth, Keys
   Loop move/len, Tape crop/FX rows) treat an ABS source as a **bipolar knob**:
   `(k-0.5)·2` fed through the same offset scaling (so "knob" on Pitch = ±24 semis
   around centre). One `cvmtx_abs` check per dest inside the existing apply switch.
4. `inputs()`: drop the `mi("K5…")..mi("K8…")` chips (they are matrix rows now);
   picks stay.
5. Setup "CV Matrix" row count (`src >= 0`) now counts the defaults — fine.

### Web (`components/rest-api/html/index.html`, then `html/convert.sh`)

- `MTX_KEYS` += `mxm`, `mxd`. `mxCell(cv, dest, amt, mode)`: a small toggle button
  after the dest select (`±` ↔ `knob`, class `mxmode`); ABS hides the range + amt
  span. `rpApply` rebuilds `mxs/mxa/mxm` from the cells. `mxReset` rebuilds from
  `mxd` (dest←default src, mode 1, amt 0; everything else off) + picks to defaults
  + the global clock pick, then posts. Rows for CV5–8 show the ABS cells where the
  K chips used to be. CSS: `.mxmode` like `.mxadd`, cyan when ABS.
- **Defaults are visibly different (Arlo):** a cell whose dest has a default and
  still matches it (same CV, ABS, amt 0) wears a small dim `default` badge after the
  toggle. Once overridden — moved to another CV, mode flipped, amount set, or the
  dest cleared — the badge becomes a **round-arrow revert button** (`↺`, class
  `mxrev`, title "back to the default (CVn, knob)"). Revert works on a working copy
  of `rpData`: put that dest back on its default CV as ABS/amt 0 (removing it from
  wherever it sits now) and re-render `rpMatrix` — no post until Apply. A default
  dest that is currently *absent* (cleared) shows on its default CV's row as a ghost
  entry (dim dest name + `↺`) so it can be brought back without hunting; the ghost is
  not a cell and does not post. The web page renders the matrix rows from `mxd`, so
  no firmware change beyond emitting `mxd`. Device page: default rows keep a `·`
  before the label while they match the default (cheap, one char).

### Docs

CLAUDE.md cvmtx bullet (ABS mode, defaults, migration, `mxm/mxd`), the "A KNOB CAN
BE ROUTED TO ITS OWN DESTINATION" note (now: the matrix IS the knob; the double-drive
can no longer happen by default), INPUT MAP para (no K chips on Synth/Keys/Tape).
HANDOFF 09-08 PM. Memory: web-remote-rework (stage 3 SHIPPED).

## Stage 4 (NOT this plan — recorded so the widget's shape allows it)

Roll `cvmtx` into the remaining machines and retire their private knob code, the
way the core clock retired their detectors: Sampler3 (bespoke matrix → cvmtx),
DoubleDecker (CV map → cvmtx entries), Deck, Looper, Tracker, Glitch, Granular,
Slicer, Drums. **Arlo (09-08): some destinations must become FOCUS-RELATIVE
rather than hard-wired** — DoubleDecker's knobs follow the selected deck, Looper's
level/pan follow the selected track, Drums' knobs follow the pad the encoder points
at. So a destination there is "loop window (focused deck)", and the host resolves
the target at apply time. Nothing in stage 3 hard-codes a target inside the widget:
a dest is just a labelled index the host interprets, which is what keeps that
possible. Stepped-ladder knobs (Deck loop length, DoubleDecker's pickup) will need
the widget to learn a hysteresis/takeover variant.

## Files

- `components/menu/include/cvmtx.h`, `components/menu/cvmtx.c` — mode/takeover/defaults/page/persistence
- `components/machine_synth/synth.c`, `synth_priv.h` — defaults, ABS apply, knob block removed, inputs()
- `components/machine_instsampler/instsampler.c`, `instsampler_priv.h` — same
- `components/machine_tape/tape.c`, `tape_priv.h` (`TPM_RES`), `tape_menu.c` (label refresh untouched)
- `components/rest-api/html/index.html` (+ regenerated `include/index.html.h`)
- CLAUDE.md, HANDOFF.md

## Sequencing (one commit each, each builds)

0. **Bail point**: tree is clean at `e9c712a` (the core-clock series, pushed, and
   the build on .85). Tag it `pre-cvmtx-abs-20260908` and push the tag before
   touching anything. Bail = checkout the tag; the .85 image is that commit.
1. Widget (`cvmtx`) with ABS mode + defaults + migration + page; no host uses it yet
   (`cvmtx_init` gains a parameter → hosts pass NULL in this commit).
2. Synth → 3. Keys → 4. Tape (with `TPM_RES`). Plain `idf.py build` each.
5. Web cells + Reset; `convert.sh`; build; `proof_build.sh` (menu component = core).
6. Docs. Then OTA `.85` (check `recording` first; `/sysinfo` after).

## Verification

- `GET /remote/params` on Synth: `mxs` shows CV5–8 on Timbre/Cutoff/Reso/Env>Cut,
  `mxm` 1 there, `mxd` = the defaults; Keys and Tape likewise (Tape `mxl` has "Reso").
- Takeover unchanged: `POST /remote/cv?ch=6&v=…` (or the panel knob) past the
  threshold → `cut` in params follows the log curve; a remote write to `cut` holds
  until the knob moves (the documented rule).
- Re-aim: on the web, change the CV6 row's dest to Level (ABS) → K6 drives `lvl`,
  `cut` stops following; add a second line CV6→Cutoff `±` +50 % → both apply.
  Reset → defaults back, offsets cleared.
- Migration: the existing autosaves on .85 (no `mxm`) load with knobs alive; Arlo's
  Keys patch with Cutoff←CV6 offset loads WITHOUT an absolute K6 (single drive).
- Clock guard: `POST /settings {"clk_src":5}` → Tape's CV6 ABS entry holds (no
  cutoff pumping); back to CV4.
- Device page: Synth > Matrix, turn amt past +100 → `knob`; back → `+100%`.
- Eyeball the Remote tab in Chrome (cells, toggle, Reset). Ear: knob feel on Synth
  and Keys should be indistinguishable from before (same threshold, same curves).
