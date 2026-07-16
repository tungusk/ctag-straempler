# Tonal Instrument Sampler — engineering design (task #24)

**Status:** design only. NOT built, NOT flashed. This is the plan.
**Date:** 2026-07-16. **Author:** design pass grounded in the shipped code.
**New file** (no prior doc existed); nothing here is superseding a decision.

Working name for the component: `machine_instsampler`.
Proposed on-device display name: **"Keys"** (short, performer-facing, unambiguous
against "Sampler"/"Sampler2" which are the loop-recorder + legacy fork). Final
name is an open question for Arlo.

---

## 1. Overview / goal

A **pitched, playable multisample instrument**: load one (v1) or several (v2)
pool samples, map them across a pitch range, and play them melodically from the
1V/oct CV with an amp+filter ADSR — the tonal counterpart to the Drums one-shot
machine. This is exactly what `machine_synth` was prototyping in its UX, but the
sound source is **real recorded samples with a sustain loop** instead of a
synthesized oscillator.

The whole machine is the **Synth's operability grammar** (four macro knobs with
drums-style takeover, a Live dashboard of labelled dials + an ADSR curve, an
8-destination CV matrix reached from Setup, 1V/oct on CV1 zeroed at the ch1 idle,
TR1 gates the ADSR) with **the Synth's `ENG_WT` wavetable path replaced by a
proper varispeed sample voice with a sustain loop.** If you understand
`machine_synth`, you understand 80% of this machine; the new 20% is the loop
engine and the zone map.

### What distinguishes it from the machines that already exist
- **vs Synth:** real samples, not oscillators; a sustain loop; a zone map.
- **vs Drums:** pitch-tracked across the keyboard (Drums is fixed-pitch one-shots),
  a sustain loop for held notes, an ADSR (Drums is decay-choke only).
- **vs Slicer:** one sample mapped *melodically* by pitch, not chopped into slices
  fired by a selector. Loops for sustain, not tails-for-length.
- **vs Sampler3 (loop recorder):** that is a beat-synced *recorder/looper* of long
  takes; this is a *keyboard instrument* played by 1V/oct with per-note envelopes.

---

## 2. v1 scope vs v2+

### v1 (build this)
- **One sample**, PSRAM-resident, pitched across the whole keyboard by varispeed.
- **Sustain loop** (loop_start/loop_end within the sample) with zero-crossing snap
  + a short wrap crossfade. This is THE feature; without it a held note runs out.
- **Mono voice** (one `is_voice_t`), reusing the Synth's linear ADSR VCA + SVF
  low-pass (env-opened) verbatim.
- **1V/oct on CV1** (same scale/zero as Synth+Slicer), quantize-to-semitone toggle,
  root-note setting. **TR1 gates the ADSR.**
- **Four macro knobs** with takeover (K5/K6/K7/K8), Live dashboard, ADSR curve, a
  waveform strip showing the loop region as a box (deck/slicer box grammar).
- **8-destination CV matrix** via Setup (Synth's matrix, sampler destinations).
- **Named preset save/load** via a shared preset-store util (see §8; overlaps #23).
- Optional output reverb (reuse `util/reverb`, lazy slab), exactly as Synth hosts it.

### v2 (hooks now, build later)
- **Multi-zone key split**: N zones, each `{sample id, lo_note, hi_note, root}`,
  each its own PSRAM slab. The voice picks the zone for the played note.
- **2-voice paraphony** (ties to task #28): TR1+CV1 and TR2+CV2 drive two
  independent `is_voice_t` with independent pitch+env, shared sample/filter/matrix.
  Then voice-stealing across >2 held notes.
- **Streaming long one-shots** (non-looped zones only) reusing the slicer/sampler3
  reader; looped zones stay PSRAM-resident by rule (loops need random access).
- **Pingpong / reverse loop modes**, per-zone loop points, loop crossfade length.
- **Velocity/accent layers** driven by a CV (there is no MIDI velocity in Euro —
  an accent CV selects among layered samples). Low priority; noted so the zone
  struct leaves room.

---

## 3. Design decisions (each with a recommendation)

### 3.1 Sample mapping model
**Recommendation — v1: single sample across the whole keyboard.** Simplest, and
it is genuinely useful because the sustain loop means a *short* recording (one
plucked/bowed/blown note of a few seconds) covers the whole range. No zone table,
no zone-select logic, one PSRAM slab.

**v2 growth: multi-zone key split** (not velocity layers first). Key splits are
what make a sampled instrument sound right across octaves — a single sample
varispeed-stretched two octaves up sounds like a chipmunk, two down like a
tape-drag. 3–5 zones removes most of that. Velocity layers are deferred because
Eurorack has **no note velocity**; the nearest analogue is an accent CV, which is
a niche the matrix can already approximate (CV→level).

**PSRAM reality (cite CLAUDE.md "single PSRAM alloc >2.1 MB is refused on this
board"):**
- Resident **stereo** ceiling per slab: 2.1 MB / 4 B per frame ≈ **524 k frames ≈
  11.9 s** @44.1 k.
- Resident **mono** ceiling per slab: 2.1 MB / 2 B ≈ **1.05 M frames ≈ 23.8 s**.
- Total PSRAM is ~64 Mbit (8 MB) but the reverb tank (~170 KB), the reader stage
  buffers, and headroom all live there too. **v2 multi-zone = several separate
  sub-2.1 MB slabs** (multiple allocs are fine; it is the *single* alloc that is
  capped), budget ~3–4 zones of a few seconds each comfortably.

**Decision:** store zones **mono by default** (a tonal instrument is usually mono;
mono doubles the residency ceiling and halves PSRAM). Offer a "Stereo" toggle per
instrument for stereo sources that matter. `sample_load(..., mono=true)` already
does the L/R average and the DMA staging — reuse it (see §5).

### 3.2 Pitch / playback engine
Same shape as the Slicer's varispeed (`slicer.c` ~L462–479) and the Synth's
`note_from_cv`:
- CV1 → semitone offset: `voct = round((cv1 - 877) / 49.0)`; the **~49 ADC
  counts/semitone** scale and the **877** idle-zero are the measured constants
  already used by Synth (`SY_CV1_ZERO`/`SY_CTS_PER_ST`) and Slicer.
- Played MIDI note = `base_note + voct` (quantize toggle rounds to semitone; when
  off, the fractional CV bends continuously).
- Playback increment: `inc = exp2f((played_note - zone_root) / 12.0f)`. At
  `played_note == zone_root` the sample plays at native rate.
- **Interpolation: cubic (4-point Hermite), not linear.** The Slicer uses linear
  because slices are transient/percussive; a *tonal* instrument holds pitched
  material where linear interpolation's high-frequency roll-off and alias images
  are audible. The convert-on-import path (`sampimport`) already chose cubic for
  exactly this reason — mirror it. Cost: 4 taps × mono ≈ trivial at 64-frame blocks.

**Range before it stutters:** because v1 zones are **PSRAM-resident (random
access, no reader in the loop)**, the Slicer's "cap up-pitch at 2× or the tail
stream starves" limit **does NOT apply here** — there is no stream. The only
ceilings are (a) Nyquist/aliasing at high up-pitch (no band-limiting, same as the
Synth WT engine — gets buzzy past ~+1 octave, acceptable), and (b) reading past
the sample end fast, which the loop handles. Recommend allowing **±2 octaves**
practical range (matches Slicer's ±24 clamp). *When v2 adds streamed non-looped
zones, THAT path re-inherits the 2× ceiling — cite the slicer lesson there.*

### 3.3 Sustain / loop points — THE key feature
A held note must not run out, so sustained playback loops a region inside the
sample.

**Model:**
```
loop_mode ∈ { LOOP_OFF, LOOP_FWD, LOOP_PP(v2) }
loop_start, loop_end   // frame indices within the (zone's) sample, loop_start < loop_end
loop_xfade             // crossfade frames at the wrap seam (e.g. 0..1024)
```
**Playback rule (LOOP_FWD):**
- From note-on, the read cursor advances by `inc` from 0 (or a start offset).
- While `env_stage != REL` (i.e. attack/decay/sustain — the note is held), when
  the cursor reaches `loop_end` it wraps to `loop_start` (`pos -= (loop_end -
  loop_start)`), so a held note sustains forever inside the loop.
- On **note-off** the envelope enters REL; the cursor **keeps looping** through the
  release tail (natural) — the note ends when the VCA envelope hits zero, not when
  the sample ends. (Alternative "one-shot release" — stop looping at note-off and
  let the tail play to sample end — is a per-instrument toggle in v2.)
- `LOOP_OFF` = one-shot: play 0→end once, envelope still gates the amp. Good for
  percussion-adjacent tonal hits.

**Zero-crossing snap (declick):** when the user sets `loop_start`/`loop_end`, snap
each to the nearest frame where the (mono) signal crosses zero **rising** (match
the *direction* at both ends so the waveform is continuous through the wrap). This
is an offline scan over the resident buffer done in the UI/adjust path (not in
`process()`), so it can walk a few hundred frames freely.

**Wrap crossfade (belt-and-suspenders):** zero-crossing snap removes the amplitude
click but not a *phase/spectral* discontinuity. Add a short **equal-power
crossfade** at the wrap (mix the last `loop_xfade` frames before `loop_end` with
the frames before `loop_start`), exactly the seam-crossfade trick Sampler3 uses on
its loop wrap (~6 ms) and the Slicer uses on fire (`SL_XFADE`). Default ~5 ms;
0 = hard loop for users who tuned zero-cross perfectly. Because the buffer is
resident this is a cheap read-two-positions-and-blend in `process()`.

**Why resident, not streamed, for loops:** a loop needs random access to
`loop_start` on every wrap. A streamed ring only holds a forward window; jumping
backward to a loop point that has scrolled out of the ring is the "phase slip"
class of bug (see the deck lessons). Rule: **looped zones are always
PSRAM-resident.** Only v2 non-looped long one-shots may stream.

### 3.4 Streaming vs PSRAM-resident
- **v1: fully PSRAM-resident.** One sample, mono, ≤~23 s (or ≤~11 s stereo). Load
  once at select time via `sample_load` (handles RAW/WAV/AIFF through `sampfile`,
  mono average, and the mandatory DMA-staging — CLAUDE.md "SDMMC DMA can't target
  PSRAM"). No reader task in v1 → the whole "reader checks run/superseded flags per
  chunk" discipline is not needed yet, which keeps v1 small (code budget is tight:
  8 MB flash / 2×3 MB OTA slots).
- **v2: hybrid.** Looped zones resident; long non-looped zones stream via a reader
  task cloned from `machine_slicer`'s (`reader_task`, unpinned, priority 6,
  `sd_lock` per burst, `MALLOC_CAP_DMA` stage buffer, aborts within a chunk on
  `!s_run`/superseded). Adopt that discipline wholesale when the time comes.

### 3.5 Voice architecture
- **v1: one mono voice.** A single `is_voice_t` (cursor, env, filter state). This
  ties to the Synth (also mono) and keeps `process()` simple.
- **Poly hook for #28:** factor ALL per-note state into `is_voice_t voice[IS_MAX_VOICES]`
  with `IS_MAX_VOICES` = 1 in v1. The instrument-level state (zone map, ADSR times,
  filter base, matrix, level, reverb) stays shared. v2 paraphony: **TR1+CV1 →
  voice[0], TR2+CV2 → voice[1]** (ch2 is the second 1V/oct jack), independent pitch
  + envelope, shared everything else. Voice-stealing (>2 notes, or CPU cap) is a
  later add on top of the array. Keeping the array from day one means #28 is "bump
  the count + add the TR2/CV2 wiring," not a rewrite.

### 3.6 Envelope / filter / modulation
- **Envelope:** reuse the Synth's **linear ADSR** verbatim (attack/decay/sustain/
  release, per-block increments computed as in `synth_process` L177–182, the
  ENV_ATK…ENV_REL state machine L215–221). TR1 falling edge = note-on/retrigger,
  rising = note-off, same as Synth L172–174.
- **Filter:** reuse `util/svf` low-pass with the env-opened cutoff:
  `fc = cutoff_base + env * env_to_cut * 5000` then `svf_coef(fc, RATE, 1.0f)` and
  the same NaN self-heal guard (`synth.c` L207–210, L263). Per-voice `svf_t` so
  poly voices filter independently.
- **CV matrix destinations** (8, mirroring `SYM_*` but sampler-flavoured):

  | idx | dest        | effect |
  |-----|-------------|--------|
  | 0 | `ISM_CUTOFF`   | + filter cutoff Hz |
  | 1 | `ISM_RES`      | + resonance |
  | 2 | `ISM_ENVCUT`   | + env→cutoff amount |
  | 3 | `ISM_LEVEL`    | + master level |
  | 4 | `ISM_PITCH`    | + pitch (semitones, e.g. ±24) — vibrato/bend on top of CV1 |
  | 5 | `ISM_START`    | sample start offset (0..len) — attack-transient stagger |
  | 6 | `ISM_LOOPMOV`  | move loop_start/loop_end together (granular-ish timbre) |
  | 7 | `ISM_LOOPLEN`  | scale loop length (loop_end − loop_start) |

  Same mechanics as Synth: per-destination `mtx_src[d]` (−1 off / 0..7 = CV1..8),
  bipolar `mtx_amt[d]`, block-rate, median-conditioned, ch1/2 rescaled from their
  tracked idle floor (`sy_mtx_cv01` → copy as `ism_mtx_cv01`). START/LOOPMOV/LOOPLEN
  are the sampler-specific destinations that make CV performance interesting.

### 3.7 Macro knob mapping (takeover)
Copy the Synth's takeover machinery exactly (`knob_capt[4]`, `knob_live[4]`,
`knob_engine`→here `knob_ctx`; a knob only takes over once moved >0.03). Layout:
- **K5 = tone/start** — engine-aware like Synth's K5: default **filter drive/tone**
  or, when a "Start" performance mode is chosen, the sample start offset. Recommend
  K5 = **start offset** (the sampler-native macro) with Setup choosing what K5 owns.
- **K6 = cutoff** (log 10 Hz..6 kHz, closes fully — Synth L121).
- **K7 = resonance.**
- **K8 = env→cut.**
This keeps K6/K7/K8 *identical* to the Synth so muscle memory transfers, and
K5 becomes the one sampler-specific macro. (Recall CLAUDE.md: only knobs 6 and 7
are fully good on this dev unit; K5/K8 are weak — takeover keeps defaults sane, and
built units get all four live. Same rationale as Synth.)

---

## 4. Data structures (C sketch)

```c
#define IS_RATE        44100
#define IS_CV1_ZERO    877         // ch1 idle -> 0 semitones (measured)
#define IS_CTS_PER_ST  49.0f       // ADC counts per semitone (sampler2 LUT scale)
#define IS_MAX_VOICES  1           // v1 mono; bump to 2 for #28 paraphony
#define IS_MAX_ZONES   1           // v1 single zone; v2 -> 6 or 8
#define IS_MAX_FRAMES  (1050000u)  // ~23.8 s mono resident, under the 2.1 MB grant

enum { ENV_IDLE=0, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL };   // as Synth
enum { LOOP_OFF=0, LOOP_FWD, LOOP_PP };                     // PP = v2
enum { ISM_CUTOFF=0, ISM_RES, ISM_ENVCUT, ISM_LEVEL,
       ISM_PITCH, ISM_START, ISM_LOOPMOV, ISM_LOOPLEN, ISM_N };

// one mapped sample region (v1 uses zone[0] only)
typedef struct {
    char     sample[24];      // pool id, <=8 chars (FatFS 8.3) but 24 for safety
    int16_t *buf;             // PSRAM-resident audio (mono int16), lazy alloc
    uint32_t frames;          // valid frames in buf (0 = empty)
    bool     stereo;          // buf is interleaved (2*frames int16) if true
    uint8_t  lo_note, hi_note;// key range this zone covers (v2; v1 = 0..127)
    uint8_t  root;            // MIDI note that plays buf at native rate
    // loop
    uint8_t  loop_mode;       // LOOP_OFF / LOOP_FWD / LOOP_PP
    uint32_t loop_start, loop_end;
    uint32_t loop_xfade;      // crossfade frames at the wrap seam
} is_zone_t;

// per-note voice state (v1: one; array leaves room for #28)
typedef struct {
    bool     active;
    int      zone;            // index into inst.zone[] chosen at note-on
    double   pos;             // fractional read cursor (frames)
    int      dir;             // +1 fwd / -1 (LOOP_PP)
    int      env_stage;
    float    env;             // 0..1
    float    freq_note;       // played note (for inc); glide target
    float    cur_note;        // glide-slewed note actually sounding
    bool     gate;
    svf_t    flt;             // per-voice filter (mono)
    uint32_t start_off;       // sample start offset (matrix ISM_START)
} is_voice_t;

typedef struct {
    is_zone_t  zone[IS_MAX_ZONES];
    is_voice_t voice[IS_MAX_VOICES];

    // shared instrument params (Setup + knobs)
    int   base_note;          // note the CV1 offset adds to (default 60 = C4)
    bool  quantize;
    float atk, dec, sus, rel; // ADSR (Synth semantics)
    float env_to_cut;         // 0..1
    float cutoff_base;        // Hz (K6)
    float res01;              // 0..1 (K7)
    float glide;              // portamento seconds (0 = off)
    float level;              // master 0..1
    reverb_t rv;              // lazy PSRAM slab (RV_OFF = bypass), as Synth

    // CV matrix (identical mechanics to Synth)
    int8_t mtx_src[ISM_N];    // -1 off / 0..7 = CV1..8
    float  mtx_amt[ISM_N];    // -1..+1
    int    cv12_floor[2];     // tracked idle floor for ch1/2

    // four macro knobs w/ takeover (Synth machinery)
    float knob_capt[4];
    bool  knob_live[4];
    int   knob_ctx;           // context the captures are valid for (-1 = recapture)

    // async load request (UI thread sets, load helper acts under sd_lock)
    volatile bool load_req;
    char  pending[24];
    int   pending_zone;
    volatile bool loading;    // engine plays silent while a zone (re)loads
} is_state_t;

extern is_state_t inst;
```

### Preset (named recall) JSON — one instrument
```json
{
  "base": 60, "quant": true,
  "atk":0.005,"dec":0.2,"sus":0.7,"rel":0.3,"e2c":0.5,
  "cut":1200,"res":0.2,"gld":0,"lvl":0.85,
  "rv":0,"rvmx":0.2,
  "zones":[
     {"s":"PIANO_A","root":60,"lo":0,"hi":127,"st":false,
      "lm":1,"ls":18000,"le":42000,"lx":220}
  ],
  "msrc":[-1,-1,-1,-1,-1,-1,-1,-1],
  "mamt":[0,0,0,0,0,0,0,0]
}
```
Built with `cJSON` exactly like `synth_preset_save`/`_load`. Sample ids inside
zones are pool ids that already obey the ≤8-char 8.3 rule; the *preset file name*
is a separate concern (§8).

---

## 5. Loading a zone (reuse, honour the SD rules)
- Selecting a sample sets `inst.load_req` + `pending`/`pending_zone`; `process()`
  sets `inst.loading` (silent) and does NOT touch SD (CLAUDE.md: `process()` does
  no SD/heap/blocking).
- A UI-thread / small helper does the load:
  `frames = sample_load(pending, zone->buf, IS_MAX_FRAMES, /*mono=*/!zone->stereo);`
  `sample_load` already resolves the id across folders/containers via `sampfile`,
  averages to mono, **stages through internal DMA RAM** (never reads SD straight
  into PSRAM), and takes `sd_lock`. This is the same call the Synth uses for its
  wavetable (`synth_load_wave`), so the pattern is proven.
- After load: default `loop_end = frames`, `loop_start = 0`, `root = base_note`,
  then run the zero-cross snapper if a loop is enabled. Clear `loading`.
- The buffer is lazy: an empty instrument costs no PSRAM (Drums-style), and a
  failed alloc fails soft (leave the zone empty, show "(none)").

---

## 6. `process()` shape (per 64-sample block)
1. `cvmed_step` all 8 CVs (median-of-5 — WiFi ADC spikes), as Synth.
2. Read the four macro knobs, apply takeover (K6 cut / K7 res / K8 env>cut / K5
   start-or-tone).
3. Track ch1/2 idle floors; read CV1 → `voct` → played note; if quantize, round.
4. Evaluate the CV matrix → effective cutoff/res/env2cut/level/pitch/start/loopmov/
   looplen (clamp each), identical structure to `synth_process` L140–168.
5. TR1 (and TR2 in v2) edge → note-on(retrig)/note-off on the voice(s).
6. Per-block: ADSR increments, glide slew of `cur_note`, filter coeff, damping.
7. Per-frame loop over `frames = MACHINE_BLOCK/2`:
   - advance envelope (Synth state machine);
   - compute `inc = exp2f((cur_note + matrix_pitch - zone->root)/12)` (clamp ±2 oct);
   - **read the sample with cubic interpolation at `pos` (+ `start_off`)**;
   - **loop handling:** if held and `pos >= loop_end` → wrap by `(loop_end -
     loop_start)`; apply the wrap crossfade over `loop_xfade`; if `LOOP_OFF` and
     `pos >= frames` → end the note (env to REL/IDLE);
   - filter (`svf_step`), VCA (`* env * level`), clamp to int16, write L/R.
8. Output reverb in place if `rv.mode != RV_OFF` (Synth L276–277).
9. `process()` reads PSRAM only; no SD, no malloc, no blocking (the loading gate
   keeps it silent while a zone swaps).

---

## 7. UI sketch (house grammar)

### Live dashboard (`M_ISMP_LIVE`)
Mirror `synth_menu.c`'s Live: redraw-on-change only (per-element signatures, no
free-running meter), black canvas, boxed elements.
- **Header:** "Keys" + zone/sample tag + **note name** (green like Synth's
  `GATE_ON`; the gate flips too fast to read as colour). `note_name()` is liftable
  as-is.
- **Waveform strip with the loop box** (the sampler-specific element, in the
  slicer/deck box grammar): grey waveform on black, a **fat box whose left/right
  edges sit AT `loop_start`/`loop_end`** (the box IS the loop window — exactly the
  Sampler3 "the box is the loop" idiom), a **white playhead** at the live cursor.
  Peaks computed once at load (like slicer `peaks[]`).
- **Four macro dials** (`dial()` is liftable verbatim): K5 start / K6 cut / K7 res /
  K8 env>cut, dim until each knob takes over.
- **ADSR curve** (`draw_adsr()` liftable verbatim).
- Footer hint line: `CV1:pitch TR1:gate K5:start K6:cut K7:res K8:env>f`.

### Setup (`M_ISMP_SETUP`) — shared `setup_menu` framework
Declared as a `setup_item_t[]` with `render`/`adjust`/`action` callbacks and the
one-line `setup_menu_event(&m, event)` handler (like `sy_setup`). Items:

| # | label | kind | notes |
|---|-------|------|-------|
| 0 | Load Sample | ST_ACTION | → sample browser → zone[0] (or selected zone) |
| 1 | Root Note   | ST_RANGE  | MIDI note that plays native rate |
| 2 | Base Note   | ST_RANGE  | note the CV1 offset adds to |
| 3 | Quantize    | ST_TOGGLE | snap CV1 to semitones |
| 4 | Loop Mode   | ST_TOGGLE | Off / Fwd / (Ping v2) |
| 5 | Loop Start  | ST_RANGE  | frames; adjust runs zero-cross snap |
| 6 | Loop End    | ST_RANGE  | frames; adjust runs zero-cross snap |
| 7 | Loop Xfade  | ST_RANGE  | wrap crossfade ms |
| 8 | Mono/Stereo | ST_TOGGLE | residency (reloads the zone) |
| 9 | Attack      | ST_RANGE  |
| 10| Decay       | ST_RANGE  |
| 11| Sustain     | ST_RANGE  |
| 12| Release     | ST_RANGE  |
| 13| Env>Cut     | ST_RANGE  |
| 14| Glide       | ST_RANGE  |
| 15| Level       | ST_RANGE  |
| 16| Reverb      | ST_TOGGLE | mode cycle (lazy tank) |
| 17| Rev Mix     | ST_RANGE  |
| 18| K5 macro    | ST_TOGGLE | Start / Tone (what K5 owns) |
| 19| CV Matrix   | ST_ACTION | → `M_ISMP_MATRIX` (Synth matrix page, relabelled) |
| 20| Preset      | ST_ACTION | → save/load browser (§8) |
| 21| Manage Zones| ST_ACTION | v2 — hidden until multi-zone |

### CV Matrix page (`M_ISMP_MATRIX`)
The Synth's `synth_matrix_handler` copied wholesale, with `mtx_labels[]` =
`{Cutoff,Reso,Env>Cut,Level,Pitch,Start,LoopMov,LoopLen}`.

### Web
Optional; the machine can publish `web_uris` later. Not required for v1 — the
on-device UI is complete on its own. Teleremote already reaches `preset_load`
for the generic settings form.

---

## 8. Storage / named preset — SHARED with task #23
Two independent layers:
- **Autosave (already exists):** the core writes `preset_save()` cJSON into
  `AUTOSAVE.JSN` under the machine name, so the last instrument state survives
  a machine switch/reboot. Free — just implement `preset_save`/`preset_load`.
- **Named recall (new, shared infra):** save/name/recall a *full* instrument to
  SD and browse a library of them. **This is the same feature task #23 wants for
  Synth patches** — build it ONCE as a util both machines call:

  `components/util/preset_store.{h,c}`
  - `preset_store_save(const char *kind, const char *name, cJSON *state)` — writes
    the machine's `preset_save()` JSON to a file.
  - `preset_store_load(const char *kind, const char *name, cJSON **out)`.
  - `preset_store_list(const char *kind, char (*out)[24], int max)` — for a browser.
  - **FatFS 8.3 / LFN-off (CLAUDE.md): the FILE NAME must be ≤8 chars.** So user
    "names" cannot be the filename directly. Store presets as
    `usr/PRE/<KIND><NNNN>.JSN` (e.g. `usr/PRE/IN0007.JSN`, `SY0003.JSN`) — an ≤8-char
    minted id via `sample_next_index`-style logic — and keep the human-readable
    name as a `"name"` field *inside* the JSON. The browser lists by the inner name,
    resolves to the short filename. (This is the exact ≤8-char lesson that bit the
    Editor; do not append a name onto the id.)
  - A tiny shared browser page (or reuse `sample_browser` pointed at `usr/PRE`) for
    pick-to-load; a "Save As" that mints the next id.

  **Call this out to whoever builds #23 first** — whichever machine ships the named
  preset browser should build `preset_store` as the shared util, and the other just
  calls it with its own `kind` tag. Do not build it twice.

---

## 9. Build / registry checklist (per CLAUDE.md "Adding a machine")
1. **New component** `components/machine_instsampler/`:
   - `instsampler.c` (engine: start/stop/process/preset_save/preset_load + the
     `machine_t machine_instsampler`), `isampler_menu.c` (Live/Setup/Load/Matrix/
     Preset handlers + `machine_ui_t`), `instsampler_priv.h`, `include/machine_instsampler.h`.
   - `CMakeLists.txt` `REQUIRES` must include **`menu`** (for `setup_menu.h`),
     plus `machine util` and whatever `synth`/`slicer` list (`driver`, `json`,
     `spi_flash`, the TFT/menusys deps). Model it on `machine_synth/CMakeLists.txt`.
2. **Registry** (`main/machine_registry.c`): `#include "machine_instsampler.h"` and
   one line `&machine_instsampler,` in `machine_registry[]` (before `&machine_stub`).
   This is the ONLY file outside the component that may name the symbol.
3. **main/CMakeLists.txt** `REQUIRES`: add `machine_instsampler`.
4. **Menu IDs** (`components/menu/include/menu_types.h`): add
   `M_ISMP_LIVE, M_ISMP_SETUP, M_ISMP_LOAD, M_ISMP_MATRIX, M_ISMP_PRESET`
   (and `M_ISMP_ZONES` for v2).
5. **Selector cap (IMPORTANT — this addition fills the arrays to the brim):** the
   registry is currently **15** entries (s2, sampler3, looper, slicer, granular,
   glitch, drums, deck, dualdeck, tracker, freesound, radio, synth, editor, stub).
   Adding this makes **16**. `menu.c` has **two** parallel `[16]` arrays each walked
   with a `< 16` guard: `s_sys_names[16]`/`s_sys_machines[16]` (L147–148, loop
   `s_sys_n < 16` L235) and `names[16]`/`machines[16]` (L288–289, loop `n < 16`
   L297). 16 entries fit *exactly* (indices 0..15) with **zero headroom** — it does
   not overflow today, but the very next machine would silently drop off the
   selector. **Bump both pairs to `[17]` and the loops to `< 17`** when adding this
   machine, so the next addition doesn't get truncated. (The *visible* selector also
   filters the 2 hidden entries — stub + Sampler2 — so on-screen it's well under the
   cap; the risk is the full-registry array, not the visible list.)
6. **proof_build** (`tools/proof_build.sh`): add `machine_instsampler` to the
   EXCLUDE list so the stub-only proof link still passes. Run `tools/proof_build.sh`
   after touching the registry.
7. **util note:** if `preset_store.{h,c}` (§8) is added to `components/util`, run
   `idf.py reconfigure` — `components/util` globs its sources and a new file won't
   link without it (CLAUDE.md svf note).
8. Build with the pinned toolchain/IDF 4.3 (`idf.py build
   -DCMAKE_POLICY_VERSION_MINIMUM=3.5`); flash via `tools/ota.sh`. Announce before
   flashing (it reboots). Do not upgrade IDF (4.4 = crackle).

---

## 10. Constraints honoured (recap, cited)
- **8 MB flash / 2×3 MB OTA slots** → keep v1 lean: no reader task, reuse Synth's
  ADSR/SVF/reverb/matrix/dashboard code, one engine path. Streaming is v2.
- **~64 Mbit PSRAM, single alloc >2.1 MB refused** → v1 one mono slab ≤~23 s; v2
  multi-zone = several separate sub-2.1 MB slabs. Reverb tank + stage buffers share
  PSRAM.
- **No CV/gate OUT** → output is audio only (the voice); nothing here needs an
  analogue output.
- **FatFS 8.3, LFN off (ids ≤8 chars)** → zone sample ids obey it already; preset
  files use minted `<KIND><NNNN>` ids with the human name inside the JSON.
- **IDF 4.3 only**, **all SD I/O under `sd_lock`** (via `sample_load`), **SDMMC DMA
  can't target PSRAM** (via `sample_load`'s DMA staging), **no core pinning for any
  v2 reader**, **reader checks run/superseded per chunk** (v2, clone slicer),
  **`process()` no SD/heap/blocking** (loading gate keeps it silent during swaps).

---

## 11. Open questions for Arlo
1. **Display name** — "Keys"? "Instr"? "MultiSmp"? (avoid clashing with
   "Sampler"/"Sampler2"). Drives the registry line + on-screen title.
2. **v1 loop-point entry UX** — is a numeric Loop Start/End RANGE in Setup + a
   waveform box on Live enough, or do you want a dedicated **loop-edit page** with
   the waveform zoomed and the two handles draggable by the encoder (nicer, more
   code)? Zero-cross snap + a small auto-loop finder ("snap loop to the longest
   stable region") could make numeric entry acceptable for v1.
3. **Named-preset ownership** — should THIS machine ship the shared
   `preset_store` util (and #23 Synth just consume it), or does #23 land first? It
   should be built once; whoever goes first owns it. Also: one flat `usr/PRE`
   folder for all machine presets, or per-machine subfolders?

(Secondary, lower-stakes: default to mono residency with a Stereo toggle — OK? and
K5's default owner: Start vs Tone.)
