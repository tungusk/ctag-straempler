# Freesound + Editor: make them work from the panel (2026-09-12)

> **STATUS: BUILT 2026-09-12, NOT YET HEARD.** Part 1 (Freesound) and Part 2.1-2.6
> (Editor) are implemented and both `idf.py build` and `tools/proof_build.sh`
> pass. **Nothing has been flashed and nothing has been played.** Step 2.7 (the
> Tape conversion) is deliberately NOT done — see "What actually landed" below.

## What actually landed

- **1.1** `components/menu/text_entry.{c,h}` — the char-picker factored out of
  `settings_input_def_handler`, which is now its first host. Long-press on `=`
  still types a literal `=` (the only way to get one into a password) and the
  wheel still opens on index 42, both preserved deliberately.
- **1.2-1.5** `fs_search_start()` parses the search on the device into
  `fsm.results[]`; query store (recents + saved) persists in the preset;
  four panel pages (query list / text entry / results / setup); `fs_safe_name()`
  shared so panel and web mint the same take id.
- **1.4** Freesound is **audible** — press a result, it fetches and plays;
  Setup has Auto-play and Drop Last.
- **NEW shared piece:** `components/util/sampplay.{c,h}` — a streaming window
  player (PSRAM ring + SD reader task, Radio's ring shape). Written because the
  plan's "load the preview into RAM" was wrong: a 90 s stereo preview is ~16 MB.
  Both machines use it.
- **2.1** Range-aware engine: `editor_apply(src, op, param, in, out)`, ops
  transform only `[in_pt, out_pt)`, plus `OP_CROP` appended (not inserted).
- **2.2-2.3** `components/menu/wave_edit.{c,h}` — the waveform strip lifted from
  Tape, **plus a zoom Tape never had**; Editor's live page is built on it.
- **2.4-2.6** Editor is audible (crop window loops while dragging); background
  peak scan; file-backed streaming clipboard (copy/cut/paste, any length);
  slice to `usr/SLICES` + `.OT` sidecar.
- **Part 3** `/edit/apply` takes `&in=&out=`, `/edit/state` reports the range,
  and the web card mirrors the panel's crop so a web "crop" means the same span.

## Why 2.7 (convert Tape) was NOT done

Two reasons, both worth keeping:

1. **Its stated precondition is unmet.** The plan says "only after 2.3-2.6 are
   playing", and nothing has been flashed yet. Converting the machine Arlo has
   actually played, against a widget nobody has heard, inverts the safety
   ordering the plan set up on purpose.
2. **Reading Tape's page changed the shape of the job.** Tape's live grammar is
   richer than the widget's: its cursor is a *button strip* that includes
   non-crop buttons, it draws `tape_eff_window()` (crop + K5/CV modulation)
   rather than the raw points, and while recording with no crop set it lights
   the whole recorded extent. The widget's four-way IN/OUT/WIN/ZOOM cycle cannot
   express that. So the conversion should take the **drawing half only**
   (`wave_edit_draw` / `_playhead` / `_x`, ~150 lines of duplicated rasteriser)
   and leave Tape's input grammar completely alone — which is where the
   divergence risk actually lives. Host-side settings that make it lossless:
   `frames = tp.cap`, `view_len = tape_view_span()`, `in_pt/out_pt` from
   `tape_eff_window()`, `cursor = -1` when the strip is not on a crop button,
   `grid_anchor = tp.in_pt`, and `in_pt=0, out_pt=tp.len` while recording
   uncropped.

## Context

Both machines are non-op in the rack. Their on-device screens are literally
status readouts that tell you to go open a browser:

- `fs_menu.c` has **one page**, no Setup, and handles no `EV_FWD`/`EV_BWD`/
  `EV_SHORT_PRESS` at all. It prints `"open the Freesound tab"`.
- `editor_menu.c` prints one big word (`IDLE`/`WORKING`/`DONE`) and
  `"pick a sample + op in the web Editor tab"`. Its Setup page has **zero rows**.
- Both `process()` functions `memset` the output to silence.

Neither has been touched since **late July** (`552e73e`, `63eed22`) — they
predate the shared `setup_menu` framework, the factored `sample_browser`, the
CV matrix and the one-page web rework. They are the last two machines still
built on the old "web-driven utility" assumption.

Arlo's rule for this pass: *a webpage-only feature is a non-starter in the rack.
The web may also drive it, but it must not be the only way.* Decisions taken
before writing this plan:

1. **Editor** gets Tape's waveform/crop grammar via a **shared wave-edit
   widget**, driven over Editor's streaming backend — any length, stereo,
   non-destructive. Tape keeps its feel and converts to the same widget.
2. **Freesound** gets **typed search plus saved/recent queries** — a shared
   text-entry widget, with the query list carrying the repeat case so typing is
   rare.
3. **Both machines make sound.** Freesound: press a result, hear it, keep or
   drop. Editor: the crop window loops while you drag IN/OUT.

Intended outcome: you can find a sound and edit a clip with your hands on the
module, and the web becomes a mirror rather than the only door.

---

## What the code already gives us (found, not assumed)

**On-panel text entry already ships.** `settings_input_def_handler`
(`~/claude09/ctag-straempler/components/menu/menu.c:494`) is a complete encoder
char-picker — `c_list = "=0123456789ABC…xyz-_ !?<^"`, turn scrolls the
character, press commits, long-press commits uppercased, `<` backspace, `^`
cancel, `=` accept, seeded from an existing value by
`menuTFTPrintAllCharSettings()`. You already type the **WiFi password and the
Freesound API key** with it. It is welded to the settings cJSON and was never
factored out.

**The original Freesound panel UI exists as reference.**
`~/claude09/ctag-straempler/components/machine_sampler2/sampler_menu.c:965-1140`
— `browse_tag_def_handler` (tag-tree browse), `browse_id_def_handler` (numeric
ID entry, same char-picker pattern), `browse_id_res_def_handler`. Unreachable
(`sampler_main_items` drops it) and its backend is commented out
(`components/freesound/freesound.c:468-476` have their `xTaskCreate` calls
commented), but it proves the shape and is worth reading before designing pages.

**Tape is already the panel sample editor** — `tape_norm/reverse/fade/copy/cut/
paste/crop_beats/snap/save_crop` plus `draw_wave` / `draw_crop_sel` /
`draw_playhead` / `frame_x` / `nudge` / the `s_grab` modal. Limits: mono, RAM
banks, ~52 s ceiling. Editor has the exact complement — a real streaming
file→file engine (`editor.c`) with no UI.

**Editing primitives that already exist and should be reused, not rewritten:**

| Need | Reuse | Path |
|---|---|---|
| read any format → int16 stereo | `sampfile_probe/read`, `sf_seek_pos` | `components/util/sampfile.c` |
| write a take | `sampwav_start/finish`, `sample_next_index` | same |
| pick a file on the panel | `sample_browser_enter_dir/event/selected` | `components/menu/sample_browser.c` |
| Setup rows + free web mirror | `setup_menu_t`, `ST_TOGGLE/RANGE/ACTION` | `components/menu/setup_menu.c` |
| one-pass peaks **+ transient envelope** from a file | `scan_file()`, `pick_transients()` | `components/machine_slicer/slicer.c:143,44` |
| read a `[start,end)` window, fwd or reverse, streaming | `read_slice_frames()` | `slicer.c:105` |
| slice-map sidecar | `slicer_build_ot()` / `slicer_parse_ot()` | `slicer_ot.c` |
| zero-cross snap | `tape_snap()` (±1024), `keys_snap_zero()` (±512) | `tape.c:116`, `instsampler.c:353` |
| background job discipline (progress, abort, `sd_lock` per burst) | `bpm_analyze()` | `components/util/bpm_analysis.c` |
| a panel list page that performs well | `more_def_handler` / `sys_row` | `menu.c:236` |
| a machine whose Live page IS a remote list | `radio_live_handler` | `machine_radio/radio_menu.c:113` |

---

## Plan

Two shared widgets are the load-bearing new code. Everything else is a host
that calls them. Both widgets follow the **`sample_browser` shape** —
`enter() / event() / result()` with the state in one file-static, one instance
at a time — because that is the pattern this codebase has repeatedly got right.

### Part 1 — Freesound (do this first)

Smaller, self-contained, and it delivers one complete use case in a single pass:
*find a sound, hear it, keep it.* It also validates the text-entry extraction
with the WiFi-password path standing by as the regression test.

**1.1 Factor the text-entry widget.**
New `components/menu/text_entry.c` + `components/menu/include/text_entry.h`:

```c
void        text_entry_enter(const char *title, const char *initial, int maxlen);
int         text_entry_event(int event);   // 0 stay, 1 accepted, 2 cancelled
const char *text_entry_result(void);
```

Lift the body of `settings_input_def_handler` verbatim — same `c_list`, same
long-press-uppercase, same `menuTFTPrintInputMenu` / `menuTFTPrintChar` /
`menuTFTPrintAllCharSettings` drawing. Then **rewrite
`settings_input_def_handler` as a thin host** so the extraction is proved
lossless on the path you already use. Do not change the character set or the
grammar in this step.

*Note on house policy:* `preset_store.h` says "no on-device text entry" — that
rule is about **ids** (FatFS 8.3, `<PFX>NNNN` auto-numbering), and it still
stands. A search query is not an id.

**1.2 On-device search.** `fs_search_handler` (`fs_web.c:46`) currently proxies
raw JSON to the browser. Split the fetch out into
`int fs_search_run(const char *q, int page)` in `fs_machine.c`, which reuses
`fs_http_get()` (already PSRAM-buffered, `esp_crt_bundle_attach`) and cJSON-
parses the 16 results into a static `fs_result_t[16]` of `{id, name, duration,
username}`. The web handler keeps proxying (the browser wants raw JSON); the
panel reads the parsed array.

> **Stack trap:** the pipeline task is `8192*5` bytes against a measured 28 KB
> peak — `fs_machine.c:26-38` records that it used to panic the module on every
> download. Do **not** hang cJSON parsing off that task or off the UI task. Give
> search its own task on the same pattern and watch `fsm.stack_min` via
> `/sysinfo`.

**1.3 Pages.** Add `M_FS_RESULTS`, `M_FS_ENTRY`, `M_FS_SETUP` to
`components/menu/include/menu_types.h`.

- **`M_FS_LIVE`** becomes the **query list**: recents, then saved queries, then
  a `+ new search…` row. Hand-rolled in the `radio_live_handler` /
  `more_def_handler` style (turn scrolls, press searches, hold → Setup).
  Header keeps the API-key and WiFi state that is there today.
- **`M_FS_ENTRY`** — four lines hosting `text_entry_*`; accept runs the search.
- **`M_FS_RESULTS`** — name / duration / user per row, big-centered-selection
  style like `sample_browser`. Press = get + audition. Hold = back. Paging by
  running off the end of the list (`&page=`).
- **`M_FS_SETUP`** — the machine's first real `setup_menu_t`: Max Duration,
  Results/Page, Auto-play, `Save Query` (ST_ACTION), `Delete Query`,
  `API Key` status, Machine affordance. Declaring `.setup` gets the web Remote
  mirror and `MC_SETUP` for free.

**1.4 Audition.** The pipeline already lands the preview in `usr/<name>.RAW`
with a `.JSN` sidecar. So: **press = fetch, then it plays**, looping, with
`keep` / `drop` on the result row (drop is one `remove()` of the RAW + JSN).
`fsnd_process()` plays from a PSRAM buffer via `sample_load()` — previews are
capped at `FS_MAX_SECONDS` 90 s so this is bounded and needs no streaming.

*Later, not now:* true stream-before-download by reusing Radio's live MP3
decoder path. Note it; do not build it in this pass.

**1.5 Persist.** `fsnd_preset_save/load` currently store only `last_query`.
Extend to `{recents[8], saved[8]}`.

**Files:** `components/machine_freesound/fs_menu.c` (rewrite),
`fs_machine.c` (+`fs_search_run`, audition, preset), `fs_priv.h`,
`components/menu/text_entry.{c,h}` (new), `components/menu/menu.c` (host
conversion), `components/menu/include/menu_types.h`.

---

### Part 2 — Editor

Staged so the machine becomes useful partway through. **Stop and play at each
stage** rather than running the whole thing before anything is heard.

**2.1 Range-aware engine.** `editor_apply(src, op, param)` →
`editor_apply(src, op, param, in, out)`. Semantics, chosen so today's behaviour
is unchanged when `in=0, out=frames`:

- `NORMALIZE` / `FADEIN` / `FADEOUT` / `REVERSE` — write the **whole file** with
  the transform applied **inside** `[in,out)`.
- `OP_CROP` (new) — write **only** `[in,out)`. This is `OP_TRIM`'s pass 2 with
  explicit bounds; near-free.
- `OP_TRIM` unchanged (it computes its own bounds).

**2.2 Panel setup rows — the machine becomes operable here.** Give Editor a
`setup_menu_t`: `Source` (ST_ACTION → `sample_browser_enter(true, "Edit Sample",
ed.src, SAMPLE_DIR_ALL)`), `Op`, `Param`, `Apply`, `Machine`. That is ~120 lines
against existing frameworks and it also lights up the web Remote mirror. At this
point Editor does everything it does today, from the panel.

**2.3 The shared wave-edit widget.** New
`components/menu/wave_edit.{c,h}`. It owns the peaks array, the view window,
IN/OUT/WIN cursor, the grab modal and the drawing; the host owns the material.

```c
typedef struct {
    const uint8_t *peaks; int n_peaks;      // host-built column peaks
    uint32_t frames, view0, view_len;       // zoom window (view_len==frames = whole file)
    uint32_t in_pt, out_pt, play;           // play == UINT32_MAX for none
    int  cursor;                            // WE_IN / WE_OUT / WE_WIN
    bool grabbed;
    uint32_t (*snap)(uint32_t frame, int dir);   // grid or zero-cross; NULL = raw
    int x, y, w, h;
} wave_edit_t;

void wave_edit_draw(wave_edit_t *w, bool full);
int  wave_edit_event(wave_edit_t *w, int event);   // 0 stay, 1 changed, 2 released
```

**Build it by lifting Tape's code** (`draw_wave`, `draw_wave_blit`,
`wave_col_desc`, `draw_crop_sel`, `draw_playhead`, `frame_x`, `nudge`, the
`s_grab` state machine in `tape_menu.c`), and **build Editor on it first, with
Tape untouched.** Convert Tape in 2.7, once the widget's shape is proven. That
ordering is deliberate: `sample_browser` was factored *from* six working copies,
not designed ahead of them, and Tape is a machine you have played and like.

Editor's peaks come from **Slicer's `scan_file()`** — one chunked pass under
`sd_lock` producing both the 300-column peak array and the transient envelope,
with progress. Zoom is the one thing Tape's version does not have (its x-axis is
always the whole tape); add it to the widget, defaulting to whole-file so Tape's
conversion is lossless.

**2.4 Audition.** Loop `[in,out)` while the crop is dragged, exactly as Tape
does. Reuse Slicer's streaming shape — `read_slice_frames()` is already "read N
playback-order frames of a window, forward or reverse" — with a PSRAM ring
filled by a reader task, refilled on crop change. A few ms of silence after a
cursor move is acceptable; this is not an instrument.

> `MALLOC_CAP_DMA` for the SD read staging — **SDMMC DMA cannot target PSRAM**.

**2.5 Clipboard, streaming.** Tape's clipboard is a PSRAM bank; Editor's should
be a **temp file** (`/raw/EDCLIP.RAW`) so it inherits the any-length property:

- `COPY` — write `[in,out)` to the temp.
- `CUT` — write the temp, and write "file minus `[in,out)`" as a new take.
- `PASTE` — write "file with the temp inserted at IN" as a new take.

All three are streaming copies with no RAM ceiling, and all stay non-destructive.

**2.6 Slice.** Over `[in,out)`, by grid division or by transient
(`pick_transients()` with a sensitivity row). Writes N takes to `usr/SLICES/`
**and** an `.ot` sidecar via `slicer_build_ot()`, so the Slicer machine loads the
map directly. This is the piece that makes Editor feed the rest of the rack.

**2.7 Convert Tape to the widget.** Only after 2.3-2.6 are playing. Acceptance
is by eye and by feel, not by test suite: the Tape live page should look the
same and the grab should feel the same. If the widget cannot express something
Tape does, **grow the widget** — do not quietly drop the behaviour.

**Files:** `components/machine_editor/editor.c`, `editor_priv.h`,
`editor_menu.c` (rewrite), `editor_web.c`; `components/menu/wave_edit.{c,h}`
(new); `components/machine_tape/tape_menu.c` + `tape.c` (2.7 only);
`components/menu/include/menu_types.h`.

---

### Part 3 — the web, afterwards

The rule this pass establishes: **nothing exists only on the web.** Both cards
survive because their panel equivalents now exist.

- Editor: declaring `.setup` makes MACHINE SETUP mirror the rows. Extend
  `/edit/apply` with `&in=&out=`. Retire the bespoke `#edcard` op-buttons
  (`index.html:794-808`, JS `3153-3187`) in favour of the mirror, keeping the
  REST endpoints for scripting.
- Freesound: keep `#fscard` — typing a query in a browser really is better —
  but point it at the same `fs_search_run()` the panel uses, and add the saved-
  query list to it.
- `MACHCARD` (`index.html:3206`) and the `BAL_*` / CSS `order` entries need to
  stay consistent with whatever is kept.

---

## Traps to carry into execution

- **`setup_menu_t.n`** — use `sizeof/sizeof`, or the `_Static_assert` Keys uses
  (`isampler_menu.c:830`). A stale literal silently drops the tail rows; it has
  bitten Tape and Synth already.
- `setup_menu_remote_json()` truncates at 4 KB ≈ 35-40 rows.
- `setup_menu_enter_at()` **draws** — never call it from a REST path or it
  paints Setup over the live page.
- One `sample_browser` at a time (the `sample_ram` shared-buffer invariant).
- `machine_ui_t.main_items` is capped at 8 entries + System.
- **Heavy repaints starve the PSRAM audio path.** Waveform on the SLOW tick
  only; use the stage-3 discipline — one `fillScreen` per full redraw with
  `CLEAR_RECT()` suppressing per-element clears, per-element signatures, and the
  `draw_wave_blit` band rasteriser rather than 300 column transactions.
- ids ≤ 8 chars (FatFS 8.3, LFN off) — `<PFX>NNNN`, never `<src>_<tag>`.
- IDF 4.3 builds with `-Werror=misleading-indentation`.
- **A crash is invisible over the network** — read `/sysinfo` `uptime`/`reset`
  after anything that "quietly did nothing".
- **Never drive `/remote/event` while Arlo is hands-on** — one cursor, and the
  zero-timeout queue send drops silently.

## Verification

Proportional to the change: build + eyeball for the UI, bench-play for
everything audible. No full suite runs for cosmetic passes.

```sh
export IDF_PATH="$HOME/esp/esp-idf-v4.3"
export PATH="$HOME/.espressif/tools/xtensa-esp32-elf/esp-2021r2-patch3-8.4.0/xtensa-esp32-elf/bin:\
$HOME/.espressif/tools/esp32ulp-elf/2.28.51-esp-20191205/esp32ulp-elf-binutils/bin:$IDF_PATH/tools:$PATH"
$HOME/.espressif/python_env/idf4.3_py3.9_env/bin/python $IDF_PATH/tools/idf.py \
    build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

- `tools/proof_build.sh` for the **shared** changes (`text_entry`, `wave_edit`,
  `menu.c` — these are core); it excludes machines, so machine-only edits still
  need a plain `idf.py build`. Never two proof builds at once.
- Web edits: `components/rest-api/html/convert.sh` **first** — its printed byte
  count is the only proof the edit landed.
- `ping ctag-modular.local` to resolve .85 (rack LAN is 192.168.3.x — resolve,
  don't remember). **Check `recording` before any flash.** `tools/ota.sh <ip>`.
- Screens by `/screenshot` (reads panel GRAM; no shadow-FB tax).

**Per-stage bench checks:**

| Stage | What proves it |
|---|---|
| 1.1 | Type the WiFi password from the panel — unchanged behaviour is the regression test |
| 1.3 | Search a query, page through results, `/sysinfo` `stack_min` stays healthy |
| 1.4 | Press a result, hear it, drop it — the file is gone from `/files` |
| 2.2 | Normalize a pool sample entirely from the panel; new take appears |
| 2.3 | Load a multi-minute file, drag IN/OUT, zoom — screenshot vs Tape's page |
| 2.4 | Crop window loops while dragging; no audio dropouts during the drag |
| 2.6 | Slice a break, then load the `.ot` in the Slicer machine and fire it |
| 2.7 | Tape live page looks and feels unchanged — by eye, by Arlo |

## Sequencing

**Freesound first** (Part 1), then Editor in order 2.1 → 2.7. Editor is useful
from 2.2 onward, so there is a playable checkpoint before the widget work
starts. Flip the order if the waveform widget is the more interesting problem on
the day — nothing in Part 1 depends on Part 2.
