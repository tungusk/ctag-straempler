# Whole-codebase code review, in chunks

## Context
The Strämpler firmware (`~/claude09/ctag-straempler`, branch `v09-machines`) has never been reviewed. The fork adds about 141k lines over `v0.9`, but most of that is vendored or generated code. Our own code is about 45k lines, which is too much for one review. This plan splits it into chunks and reviews all of them first, with no fixes along the way. Every finding goes into one list that gets triaged afterwards.

Decisions made: **review everything, fix later** · **`medium` depth everywhere** · extras: **our TFT library commits only**.

## How each chunk runs
- One chunk per `/code-review medium <paths>` run, with the chunk's directories and files as the path target. No `--fix`.
- After each run, copy its findings into the tracking file under that chunk's heading: file:line, a one-line summary, the failure scenario, and CONFIRMED or PLAUSIBLE. Then tick the chunk.
- Several chunks can go in one sitting. There is no bench step between chunks, and nothing is flashed.
- **Tracking file:** `plans/code-review-2026-09.md` in the firmware repo, next to `worth-hearing.md`. It holds the chunk checklist, the findings for each chunk, and a triage section at the end.

## Excluded
- Vendored: `components/libxmp`, `components/helix`, `components/shine`
- Generated: `components/rest-api/include/index.html.h`
- Web page source: `components/rest-api/html/index.html`, which you didn't pick
- Not built: `components/machine_sampler2`, excluded in the top-level `CMakeLists.txt`
- Upstream TFT library code and its font `.c` files
- The `hardware/` submodule, `tools/`, bench logs, `.wav`/`build`/`bin`

## Chunks
Shared code comes first. A bug there reaches every machine, and knowing it makes the machine findings easier to read. Line counts are approximate.

| # | Chunk | Paths | ~Lines |
|---|---|---|---|
| 1 | Audio engine + machine core | `components/audio`, `components/machine`, `main` | 3.5k |
| 2 | Shared DSP + FX rack | `components/util`: reverb, flanger, fxdelay, overdrive, lfo, pitch_detect, bpm_analysis; `components/fxrack` | 2.5k |
| 3 | Sample and file I/O | `components/util`: sampfile*, sample_ram, sampimport, mp3, fileio, list, preset_store, sampplay, sample_list_recent, string_tools, the timer/time utils, disp_lock, plus any other util files | 2.5k |
| 4 | Menu framework | `components/menu`: menu.c, menutft.c, menusys.c, menu_config.c, setup_menu.c, headers; `components/ui` | 2.4k |
| 5 | Shared widgets | `components/menu`: cvmtx, wave_edit, text_entry, sample_browser | 1.9k |
| 6 | Network + REST | `components/rest-api/rest-api.c`, `components/wifi`, `components/freesound` | 3.6k |
| 7 | TFT library (our commits) | submodule `components/tft/ESP32_TFT_library`, diff of our commits (`a1890d0`^..HEAD; confirm the base with `git log` first) | small |
| 8 | Samplers A | `machine_sampler3`, `machine_slicer` | 3.8k |
| 9 | Samplers B | `machine_instsampler` (Keys), `machine_drumsampler` | 4.6k |
| 10 | Decks | `machine_deck`, `machine_dualdeck` | 4.2k |
| 11 | Tape + Looper | `machine_tape`, `machine_looper` | 3.5k |
| 12 | Synth-type machines | `machine_tracker`, `machine_synth`, `machine_granular`, `machine_glitch` | 4.0k |
| 13 | Streaming + editing | `machine_freesound`, `machine_editor`, `machine_radio` | 3.7k |

If a run turns out too big or its findings look thin, split that chunk in two before continuing (for example 9 → Keys / Drums).

## After all 13: triage
- Merge duplicates. The same shared-code bug will likely show up through several machines.
- Check each finding against memory before calling it new. Many areas already have notes: the reverb pop, the FX combo dirt (six hypotheses already ruled out), the lwIP socket pool, the Freesound download faults, the text-entry prompt collision.
- Sort into **fix** / **watch** / **not a bug**. Rank the fixes by consequence: crash and memory faults first, then audio glitches, then UI.
- The fixing itself is a separate, later plan, done one machine at a time with the bench in the loop.

## Verification
This plan is reading only, so there's nothing to build or flash. It's done when all 13 chunks are ticked in `plans/code-review-2026-09.md` with their findings recorded, and the triage section is filled in.
