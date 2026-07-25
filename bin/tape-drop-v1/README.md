# tape-drop-v1 (2026-07-25)

Full firmware snapshot at `tape-drop-v1-20260725`. A Tape session (crop drop,
clock/knob collision, Live UI) plus a sample-browser crash fix found on the way
in.

Serial-flash (recovery only — normal updates are `tools/ota.sh <IP>`):

    ./flash.sh [/dev/cu.usbserial-XXXX]

OTA layout (app @ 0x20000). Flashing an OLD single-app archive (e.g.
`bin/synth-v1`) reverts OTA and needs a re-migration.

## What's in it

**Sample browser: KEYS/ and TAPE/ folder rows crashed the module**
- `components/util/sample_list_recent.c` kept its OWN copy of the pool folder
  list (separate translation unit because `ff.h` and `dirent.h` both typedef
  `DIR`) with only 5 entries, while its loop already ran to `SAMPLE_DIR_N` = 7.
  Pressing `KEYS/` or `TAPE/` in any dated browser indexed past the end of the
  array and handed `f_opendir` a wild pointer. This is the 2026-07-18 `SF_DIRS`
  bug's twin — that fix patched `sampfile.c` and missed this private copy.
  Affected the dated browsers (Sampler, Drums, Deck, DoubleDecker, Tape, Keys);
  Granular/Synth/Slicer use the alphabetical browser and were fine.
- `components/util/sampfile_f.c` — `SF_DIRS_F` was missing KEYS/TAPE too. No
  crash (its bound derives from the array), but the raw-FatFS resolver used by
  the streaming readers could not find an id living only in a machine home
  folder, so it fell through to `usr/<id>.RAW` and failed to open.
- All FOUR parallel folder arrays now carry `_Static_assert(... == SAMPLE_DIR_N)`
  so the next folder bump is a build error instead of a runtime crash. That
  gotcha had bitten three times.

**Tape: Crop = save the loop, then crop to it (Arlo)**
- `tape_save_crop()` writes the looped area to a fresh `usr/TAPE/TCR_NNNN.WAV`,
  and `tape_drop_adopt_kick()` (menu tick, once the writer closes the file)
  loads that file back as the tape's content. So one press both banks the loop
  and crops down to it; playback resumes from the top if it was rolling.
- **Nothing is lost.** The adoption goes through `tape_load()`, which persists an
  unsaved take WHOLE (a `CUT_` file) before replacing the buffer — so the
  material outside the loop is on the card even though the tape now holds only
  the crop. A spoiled file (see the overwrite detector below) is NOT adopted.
- **Fires while playing.** It used to require a stopped transport, and the Live
  button row skipped it in the encoder scroll while rolling, so mid-loop the
  button could not even be selected — which is why it looked like it did
  nothing. Only RECORDING is refused now (the audio task owns the buffer then).
- Saves the **effective** window (`tape_eff_window`), so K5 window-move and the
  CV matrix's In/Out/Window modulation are included: what you hear looping is
  what lands in the file.
- New `adopt` flag on `tape_spawn_save()`. The drop passes `false`, so it no
  longer hijacks `restore_id` or clears `take_dirty` — previously a crop save
  silently repointed `tapelast` at the excerpt and marked a still-unsaved full
  take clean, so dropping a loop could cost you the rest of the take on the next
  machine switch. The three auto-save paths pass `true` and are unchanged.
- Confirmation on the Live page (`>TCR_0007...` while writing, solid when
  closed, ~6 s) naming the file that was written. Live button stays "Crop";
  Setup row is "Crop Loop". (It read "Drop" for part of the session, while the
  function only copied to disk and left the tape alone.)
- **Overwrite detector**: the writer reads the bank in the background for ~1 s,
  and an overdub punch during that window would blend new audio into the file.
  The audio task now flags a write inside `[save_a, save_b)`; the note shows
  amber `!TCR_0007` and the log warns, and the crop is NOT adopted (the tape
  keeps its current content). It does not block the gate — that would cost the
  downbeat — it just never lies about it.

**Tape: clock was audibly modulating the audio with every FX slot Off**
- CV5-CV8 are knob+jack channels, so a clock patched into one is read twice —
  by `clockin_block()` AND by the knob-takeover map. Tape's default clock source
  is **CV8**, which was mapped to **K8 -> drive**; the pulse train pushed drive
  past the 0.03 takeover threshold and latched, and a soft clipper engages at
  `drive > 0.005f`. Hence rhythmic distortion with no FX enabled.
- Second-order: each pulse also tripped `machine_state_dirty()`, so AUTOSAVE.JSN
  was written to the card continuously while clocked — SD contention under
  `sd_lock` during playback. Worth revisiting as a lead on the open Drums click.
- **CV8 is un-wired from Drive** (`TP_N_KNOBS = 3`, Arlo's call); Drive is a
  Setup row only. K5/K6/K7 keep window-move/cutoff/reso, and whichever channel
  is the clock source is excluded from the knob path so CV5/6/7 can't recreate
  it. The CV matrix can still be aimed at the clock channel — that is an
  explicit user assignment, not a hidden default.

**Tape Live UI**
- Encoder CW order through the loop elements is now START > WINDOW > END.
  `nudge()` dispatches on the element rather than its enum position.
- Loop box: a 3px green outline around the loop area while Window is selected
  (brighter while grabbed), drawn per-column inside `wave_col` so the playhead
  erase cannot punch holes in it. 1px cyan edge ticks at IN/OUT stay on always;
  selecting IN or OUT still bolds that edge.
- Rev/Norm/Fade are drawn extra-dim while the transport runs, so the row shows
  what can actually fire.

## Verification

Built clean (no warnings) and `tools/proof_build.sh` passes. Browser fix and all
Tape changes were flashed and exercised on hardware during the session; the CV8
diagnosis was confirmed by Arlo (clock patched to CV8, matching the predicted
mechanism).

EAR-TESTS OWED (shipped ahead of them on purpose): the crop's audio on a
monitor, the load-back timing while rolling, the `CUT_` full-take safety file
landing when cropping an unsaved recording, and the overwrite-detector's amber
path (needs a deliberate overdub during the ~1 s write). The clock fix wants a
listen with a clock patched to CV8 and the drive stage confirmed quiet.
