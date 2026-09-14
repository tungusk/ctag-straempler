# Long filenames: getting off the 8.3 cap (2026-09-13)

> **STATUS: DONE — `4722ac9`, flashed and verified on .85.** Bail point
> `pre-lfn-20260913`. Spike numbers, the two regressions it caused and the
> display decisions are all in the commit message. Outstanding: Step 6
> (`pitch_name_hint` re-look with real long names) and an ear check that
> streaming machines are unaffected — the mechanism says they cannot be
> (LFN allocates on `f_open`/`f_readdir`, never `f_read`) but nobody has
> listened.

## Context

Freesound fetches were failing because `fs_safe_name()` truncated to 12
characters and the card is FatFS 8.3 — `f_open("/usr/Tabla-Downwa.RAW")` returns
`FR_INVALID_NAME`. Fixed at `91d214d` by capping at 8, which works but means a
sound called *Tabla-Down-126bpm* lands in the pool as `Tabla-Do`.

Arlo asked whether the 8-character limit is a FAT32 thing or something we can
overcome.

**It is not FAT32.** FAT32 has supported long names via VFAT since the 90s. Long
filename support is simply compiled out of our FatFS:

```
CONFIG_FATFS_LFN_NONE=y
# CONFIG_FATFS_LFN_HEAP is not set
# CONFIG_FATFS_LFN_STACK is not set
```

Turning it on is a `sdkconfig` change. The work is not the flag; it is that
20-odd files size their name buffers at a hardcoded `24` and the panel draws
names assuming they are short.

**Nothing on the card is precious** (Arlo, 2026-09-13: *"everything we've ever
done is a test. nothing is precious"*). So this plan does not carry migration,
does not preserve the existing pool, and does not spend a step proving old 8.3
files still resolve. If reformatting the card is ever the simpler route, take
it. That removes the usual reason these transitions drag.

Intended outcome: a sample keeps the name it arrived with, everywhere — pool,
browser, machine live pages, web — with the streaming audio paths measurably
no worse than they are today.

## What only changes for SOURCE-derived names

Worth being clear, because it bounds the whole job. Ids the firmware **mints**
are auto-numbered and stay 8 characters by design, unaffected:

`REC_NNNN` · `BNC_NNNN` · `LOOP_NNNN` · `TAP_NNNN` / `TCR_` / `CUT_` ·
`SLC_NNNN` · `NM_`/`RV_`/`FI_`/`FO_`/`TR_`/`CR_NNNN` · `PAT_NNN` patches

Only names derived from a **source** get longer: Freesound titles, uploads and
`/import`. So the payoff is concentrated exactly where the pool is hardest to
navigate — 289 files today, most of them auto-numbered.

---

## Step 1 — SPIKE FIRST, and be willing to stop

Flip the flag, change nothing else, and measure. This step exists because the
risk is not correctness, it is **allocation churn on the audio path**.

`CONFIG_FATFS_LFN_HEAP` allocates a work buffer per path operation.
`CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` is already set, so those land in **PSRAM** —
and PSRAM traffic during streaming is precisely what produced audio clicks
before (see `project_http_audio_glitch_20260727`, `reference_sd_memory_gotchas`).
Slicer tails, Deck, Tracker and the new `sampplay` all open/read on the card
while audio runs.

- Set `CONFIG_FATFS_LFN_HEAP=y` and `CONFIG_FATFS_MAX_LFN=64` (not the 255
  default — it quarters the buffer and 64 is far beyond any name we want).
- Do **not** pick `LFN_STACK`: it puts 512+ bytes on the caller's stack, and we
  have just spent a session fighting stacks and a 10 KB largest-free-block.
- Measure, on the bench, before touching any other file:
  - `idf.py size` delta, and `/sysinfo` `iram`/`psram` free + largest block
  - **an audio soak**: Slicer firing streamed tails, Deck playing, Tracker
    running — listening for clicks, and watching `aus`/`auspk`/`ausgap`
  - a `/files` listing of the full 289-file pool for time and memory

**If the soak is dirty, stop here and report.** A nicer filename is not worth a
click in the audio path, and the `SAMPLE_ID_LEN` refactor below is wasted work
if the flag has to come back off.

## Step 2 — one constant instead of twenty literals

The reason this is a real job: the name length is written out as a bare `24` in
about twenty places. Introduce it once, in
`~/claude09/ctag-straempler/components/util/include/sample_ram.h`:

```c
// Pool ids are no longer FatFS 8.3 short (LFN is on). ONE definition, because
// this used to be a hardcoded 24 in every machine and every list buffer.
#define SAMPLE_ID_LEN 32
```

**32, not 64**: the cost is paid per entry in two 512-slot tables, so each extra
byte is ~1 KB of PSRAM in `sample_list_recent` and another ~0.5 KB in the REST
file index. 32 covers real sample names; 64 doubles the tables for names nobody
types.

Then convert, mechanically:

| Where | Today |
|---|---|
| `components/util/include/sample_ram.h` | `sample_list(char out[][24])`, `sample_list_shared/recent(char (**out)[24])`, both `_dir` variants |
| `components/util/sample_list_recent.c` | `char (*list)[24]`, `char id[24]`, `char tmp[24]` |
| `components/menu/sample_browser.c:36` | `char (*list)[24]` |
| `components/rest-api/rest-api.c:194` | `fidx_t.id[24]` (× `FIDX_MAX` 512, PSRAM) |
| per-machine | `DK_NAME_LEN` · `DD_NAME_LEN` · `S3_NAME_LEN` · `TRK_NAME_LEN` · `ED_NAME_LEN` (all 24), and the bare `char sample[24]` in granular, drums, slicer, plus tape's `restore_id[24]` |
| `components/machine_freesound/` | `cur_name[24]`, `fs_job_t.name[24]`, `s_au_name[24]` |

**Leave alone**: `tape_priv.h` `save_id[12]`/`adopt_id[12]`, `preset_store_list(char ids[][12])`,
`keys_patch_list(char ids[][12])`. Those hold minted ids, which stay short.

## Step 3 — the write side

- `fs_safe_name()` (`fs_machine.c`): raise the cap from 8 to `SAMPLE_ID_LEN-1`
  and keep the alnum/`_`/`-` filter. **Keep rejecting the characters FatFS still
  dislikes** — LFN permits more, but `*?<>|:"/\` stay out.
- Web card: three places still say 8 after `91d214d` — the `substring(0,8)` in
  `fsRender`, the `fsGet` prompt text and filter, and `fsFetch`'s default.
- `components/util/sampimport.c` and the Upload path mint names too.

## Step 4 — the panel has to draw them

This is the part that is easy to forget and immediately visible.

- `sample_browser.c` centres the selection in `DEJAVU24_FONT` and the neighbours
  in the default font, with **no width clamp** — a 32-char name will run off
  both edges. Truncate with an ellipsis, and/or drop the selection to a smaller
  font when the longest name in view does not fit (see the decision above).
  `printSubStringIfTooWide()` (`menutft.c:540`) is the existing prior art.
- Machine live pages that print the loaded id: Tape's big filename row
  (`header_name`), Slicer, Deck/DoubleDecker, Keys' zone strip, and the
  Editor/Freesound headers I added yesterday (those already trim with a
  `TFT_getStringWidth` loop — reuse that shape).

## Step 5 — REST and web

- `/files` already streams per folder; only `fidx_t.id` widens.
- `/files/rename` and `/files/move` validate names — relax to match the new
  filter, and make sure the web input `maxlength` agrees.
- `sample_resolve` / `sample_resolve_aux` build into `path[80]`; with a 32-char
  id plus `/sdcard/usr/SLICES/` plus `.AIFF` that still fits, but check rather
  than assume.

## Step 6 — the one behavioural side effect

`pitch_name_hint()` (`components/util/pitch_detect.c:41`) pulls a note name out
of a sample id and its comment says outright: *"Sample ids are FatFS 8.3 short
(<=8 chars), so the note is usually crammed against the instrument name
(PNOC4)"* — hence its `isolated` flag. Longer names should make Keys' auto-tune
hint **better**, not worse, but the heuristic was tuned against cramped ids and
deserves a re-look with real long names before it is trusted.

---

## Verification

Proportional: build + eyeball for the drawing, bench-play for the audio claim.

- Build: the usual IDF 4.3 invocation; `tools/proof_build.sh` for the shared
  changes (`sample_ram.h` is core), plain `idf.py build` for machine-only edits.
- `html/convert.sh` after the web edits — watch the byte count.
- **The gate is Step 1's soak.** Re-run it after Step 2 as well, since the
  widened tables change PSRAM pressure.
- End to end: fetch a Freesound sound with a long title and confirm it lands
  whole, shows correctly in the browser and on the machine page, survives a
  reboot (autosave stores the id as a JSON string — no length assumption), and
  loads into Slicer/Keys/Tape.

## Decided (Arlo, 2026-09-13)

1. **`SAMPLE_ID_LEN` = 32.**
2. **Truncate by default, with a smaller font allowed where a machine needs the
   whole name.** Arlo, revising: *"in case of machines showing large filenames,
   they can probably shrink the text to 50% if needed, which will create the 4x
   space to accomodate the jump from 8-32 char."*

   So: clip with an ellipsis as the baseline (measure with `TFT_getStringWidth`,
   trim from the tail — the shape already used in the Freesound/Editor headers),
   and where a page really wants the full name, halving the glyph size buys
   ~2x the characters on one line, or ~4x the area if it wraps to two.

   **The one rule to keep:** pick the size from the LONGEST name currently in
   view, not per-name. A name that changes size as you scroll past it reads as a
   glitch; a list that settles on one size for the whole screen does not. Same
   for Tape's big filename row — it should not resize as you load different
   takes mid-set, so choose per-page and stay there.
