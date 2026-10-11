# Handoff 2026-09-30 — fixing the 09-29 code review

For the next session (Opus). Written by the Fable session that ran the review; Arlo was out of
Fable budget for the week, so the fixes move here.

## Where things stand
- Repo `~/claude09/ctag-straempler`, branch `v09-machines`, HEAD `8750c96`, clean tree apart from
  untracked files. The bench unit `.85` (`stramp.local`) runs `e594288`+, same code as HEAD.
- The review is DONE and triaged: `plans/code-review-2026-09-29.md` (themes A–Z, three tiers).
  Full verifier reasoning and a fix sketch per finding: `plans/code-review-2026-09-29.json`,
  keyed by `#id`. Both untracked. **Nothing is fixed.**
- An appendix at the end of THIS file repeats the brief for every finding in the themes below,
  so you need not parse the JSON. To look one up anyway:
  `python3 -c "import json;r=json.load(open('plans/code-review-2026-09-29.json'));print([x for c in r['chunks'] for x in c['results'] if x.get('id')==97])"`
- ✔ in the triage file = the Fable session read the cited code itself and it is as described.
  Unmarked = verifier only. (P) = plausible: real defect in the code, trigger not established.
  Every fix still starts by reading the code around the finding; the line numbers are from
  `8750c96` and the JSON's "claim"/"scenario" fields are the finder's words, which the verifier
  sometimes corrected in "reasoning" — trust reasoning over claim.

## Arlo's calls for this work (09-29)
- **Stop-and-show was for the triage; that is done.** At session start, confirm with Arlo which
  themes to take. Fable's suggestion, in order: **O, P, L first** (reverse, the dead web cards,
  Tape data loss — all hit in normal use), then **tier 1 A–H**, then J, K, M, N, Q, S, T, X, Z.
- **Fix by theme**, one commit per theme on `v09-machines`. No push unless Arlo says. No flash
  without announcing it first, and never while `recording` is set.
- **Helper model by surface.** Arlo's 07-07 note: a Sonnet session broke audio on everything it
  touched. So: Sonnet helpers only for `index.html` and display-text-only edits (themes P, Q, R and
  the page half of S); you (Opus, full model) do anything in audio, DSP, tasks, memory, SD or REST
  handlers yourself or via a full-model helper. Either way the brief states the exact defect, the
  exact change, the prior art to reuse, what not to touch, and that the helper must not build,
  flash, commit or contact the unit. Read every diff.
- One helper at a time in the main tree (parallel only on disjoint file sets). No worktrees: each
  would need its own IDF build dir.
- "Nothing is precious": card data is test material, no migrations. Repo history, a machine's
  feel and hardware are not covered by that.

## Build, prove, ship
```sh
export IDF_PATH="$HOME/esp/esp-idf-v4.3"
export PATH="$HOME/.espressif/tools/xtensa-esp32-elf/esp-2021r2-patch3-8.4.0/xtensa-esp32-elf/bin:$HOME/.espressif/tools/esp32ulp-elf/2.28.51-esp-20191205/esp32ulp-elf-binutils/bin:$IDF_PATH/tools:$PATH"
$HOME/.espressif/python_env/idf4.3_py3.9_env/bin/python $IDF_PATH/tools/idf.py build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```
- The `idf.py` on PATH is IDF 6.2 and FAILS. Only the line above works. The build command's own
  exit code is the gate; never grep the log for success.
- IDF 4.3 builds with `-Werror=format-truncation` and `-Werror=misleading-indentation`: cap display
  `snprintf`s of grown id buffers with `%.Ns`; no `break` trailing an `if` on one line.
- **Web edits: run `components/rest-api/html/convert.sh` FIRST**, and check its printed byte count
  moved. CMake does not run it; an unconverted edit silently does not ship.
- Core, registry or `components/machine` edits: `tools/proof_build.sh` must pass. **Never two at
  once** (it swaps `main/machine_registry.c` and `main/CMakeLists.txt`; a collision leaves a
  one-machine registry that builds and boots fine — recover with `git checkout -- main/`).
- `components/util` globs its sources: a new `.c` there needs `idf.py reconfigure`.
- Flash = `tools/ota.sh stramp.local` (OTA, ~14 s, reboots). Announce first. Check
  `GET /sysinfo` `recording` before, and `uptime`/`reset` after: a crash is invisible over the
  network. Stop Radio before OTA ("oom"). `ota_begin failed` = fragmented RAM, `POST /reboot`.
- Bench after flashing: `tools/bench/review_bench.py --audio --reboot` (switch / Keys / Tape /
  Synth / autosave / uri / stall / import / lfn tests; `--serial /dev/cu.usbserial-3120` greps
  for leaking / still running / panic). It snapshots and restores the patches it touches.
  Don't poll `/status` while Arlo is judging audio; REST while he is hands-on collides.
- Ear and panel tests go on `plans/worth-hearing.md` and the triage file, they don't block a
  commit. Judge Deck/DoubleDecker only ~60 s after a load.

## Notes from reading the code, per theme (beyond the appendix)
- **O (reverse).** `read_playback_frames()` in sampler3.c and `read_slice_frames()` in slicer.c
  are called with `dst == stage` on the head, loop-start-cache and slice-head paths, and with a
  different `dst` on the ring path. The in-place branch must be a swap
  (`for i < got/2: swap frame i and got-1-i`), the out-of-place branch can stay a reversed copy.
  Keep both paths; don't allocate a second stage buffer (internal RAM). The thumbnail pass also
  goes through it. Ear test owed afterwards: Sampler reverse and Slicer reverse on a known take.
- **P (web cards).** The elements `#rdnote`, `#ednote`, `#ptag` were lost in the 09-11 one-page
  rework (`web-one-page-20260911`). Add the elements back inside the Radio / Editor cards and the
  header rather than guarding the lookups, so the notes show. Any `innerHTML` re-render of a
  folded card destroys its `makeFold` wrapper: re-fold whatever you re-render. The page can be
  tested live before flashing by serving the edited functions from the Mac and `eval`ing them
  (recipe in memory `build-flash-loop`); `/screenshot` and the live page beat guessing.
- **L (Tape).** `tape_copy()`/`bank_alloc()` return 1 for trimmed; `tape_cut()` must refuse on
  anything but a full clipboard (#97). For #100, the dirty flags are `take_dirty`, `dirty_gen`,
  `restore_id` — set/bump/clear in each edit that changed the buffer; `save_task` clears
  `take_dirty` only when `dirty_gen` is unchanged since it started (that is the 11.5 fix, don't
  undo it). Tape's process() runs in the audio task: no SD, heap or blocking there; the writer is
  spawned from the UI tick. #98 (theme E) touches the same file: do E in the same sitting.
- **A (staging buffers).** Allocate in `start()` before `worker_spawn()`, pass through the worker
  arg, return `ESP_ERR_NO_MEM` on NULL so `machine_activate()`'s Stub fallback engages; free in
  `stop()` only after `worker_stop()` succeeded (leak on timeout — `util/include/worker.h`).
  `tape.c:620` and `sampplay.c:57` are the checked prior art.
- **C (UI self-block).** `menuBindMachineUI()` is called from the UI task itself during a switch.
  Send with timeout 0, and have `menuProcessEvent` run `menuMachineBindNow()` first whenever
  `s_bind_pending` is set and any other event arrives; keep EV_MACHINE_BIND a no-op when nothing
  is pending. The 300 ms timer tick guarantees a prompt bind. Don't bind synchronously inside
  `menuSwitchMachine`: `menuMachineBindNow` frees `_ms` under the running handler.
- **D (fxrack).** Bounds-check `param` against `gen_desc[k].np` (and NULL `.p` for FXK_OFF) in
  `fxrack_menu_val/adj`, and queue `EV_ENTERED_MENU` after `EV_REMOTE_PRESET` as
  `EV_REMOTE_SETUP` already does, so the page rebuilds its rows. For #12, order `fxrack_load`
  like `slot_set`: init the slab, then publish the kind; in `fxdelay_init`/`flanger_init` store
  `bufL` last.
- **G (MP3).** `mp3.c` `input`/`readPtr` are globals shared by the async decoder and
  `decodeMP3FileSync` — make them locals of the sync path, and add one static mutex around the
  body of `samp_import_file()` (both callers are background tasks). `MP3FindSyncWord(input,
  nRead)` not `MAX_FRAME_SIZE`. On a frame error, resync like `radio.c:312-318` or report failure
  so the source is kept; check `f_write`.
- **J (JSON writes).** `imp_publish()` in sampimport.c is the park-and-rename prior art for
  `writeJSONFile`. Make it return int, and let `autosave_now`/`preset_store_save` act on it.
  `sd_lock` is recursive, so wrapping a whole read-modify-write in it is legal.
- **H/K/Y (clamps).** The panel rows already hold the ranges (e.g. `granular_menu.c:132-135`);
  clamp in `preset_load` to the same numbers. `cJSON` `valueint` of a string is 0 and
  `valuestring` of a number is NULL — `/remote/params` lets the page send either.
- **Long filenames** are ON: `SAMPLE_ID_LEN` is 32. `CLAUDE.md`'s "ids ≤ 8 chars / LFN OFF"
  paragraph is stale, as is "svf is Chamberlin" and "no /reboot endpoint" — don't be misled, and
  fix those three paragraphs when you next touch CLAUDE.md.
- Known, don't re-find: reverb pop (memory `reverb-pop`), FX combo dirt (six hypotheses ruled
  out), lwIP socket pool, text-entry prompt collision, Deck's ~60 s settle after boot.

## Tracking
Record per-theme status in `plans/code-review-2026-09-29.md` under each theme (commit hash, what
was bench-run, what is owed by ear or by hand), the way the 09-16 file did. Update memory
`project_code_review_20260929.md` at the end of the session.

---

# Appendix — fix briefs per finding (verifier text, from the JSON)

### O. Reverse is a mirror-fold (Sampler3 + Slicer)

**#56 CONFIRMED · wrong-audio · normal-use · `components/machine_sampler3/sampler3.c:107`** — Reverse mode: in-place chunk reversal (dst == stage) folds every chunk into a palindrome

- Why: All four callers (107, 126, 325, 375) pass stage as both dst and stage, and the reverse loop at 76-80 copies stage[got-1-i] to stage[i] with no temp. For i >= got/2 the source frame was already overwritten, so the result is the reversed second half followed by the unreversed second half; the first half of each file chunk is never heard. Present since the first commit 3a6298c (07-12); reachable from the Setup Reverse row (sampler3_menu.c:460) and preset rev (now via s3_set_reverse after 8.4). Head, ring, loop-start cache and thumbnail are all affected.

- Fix: In read_playback_frames reverse branch, swap frame pairs in place (i < got/2, two-ended swap of L and R) instead of the one-way copy; the dst parameter can then be dropped since every caller memcpy's out of stage anyway. _[io-rest]_

**#63 CONFIRMED · wrong-audio · normal-use · `components/machine_slicer/slicer.c:201`** — Slicer reverse heads are flipped in place (dst == stage) and come out as palindromes

- Why: build_heads passes stage as both dst and stage (slicer.c:201-202); hl <= SL_HEAD_FRAMES 3528 < SL_CHUNK 4096, so got_total is 0 and the flip loop at slicer.c:130-133 reads and writes the same buffer. For i < got/2 it writes orig[got-1-i]; for i >= got/2 it reads back what it already overwrote, so the second half stays forward: each reverse head is the last ~40 ms reversed followed by the same 40 ms forward, and the first half of the chunk is lost. Forward mode is fine (memcpy onto itself at offset 0). Not in the 09-16 review; the same pattern sits in sampler3.c:76-80 with callers at 107/126/326/375, in the code since 419344b.

- Fix: In read_slice_frames, reverse the just-read chunk in stage with a two-ended swap over got/2 frames, then memcpy to dst only when dst != stage. One change there fixes both call sites; apply the same change to sampler3's read_playback_frames. _[io-rest]_

**#64 CONFIRMED · wrong-audio · normal-use · `components/machine_slicer/slicer.c:295`** — Slicer reverse tail chunks are flipped in place (same root cause as #0)

- Why: Same defect as #0 at the tail call site: slicer.c:295-296 passes stage, stage with want <= SL_CHUNK, so the flip at slicer.c:130-133 aliases and every 1024/4096-frame chunk copied into sl.ring (302-304) is reversed-upper-half followed by forward-upper-half. With Reverse ON (menu row 5, slicer_menu.c:355, or preset 'reverse') the body of every slice longer than the head is garbled. Not a separate bug; it is fixed by the #0 fix.

- Fix: Covered by the #0 fix in read_slice_frames (in-place swap reversal in stage, memcpy only when dst != stage); no change needed at this call site. _[io-rest]_


### P. Web cards that throw on a missing element

**#41 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/html/index.html:2750`** — rdState() throws on missing #rdnote; Radio card never renders state or stations

- Why: #rdcard (lines 787-800) has rdstate/rdinfo/rdbuf/rdstations but no rdnote; it was dropped in 89be785 (09-10) when the tab became a card. The first statement of the .then throws, the .catch at :2774 dereferences the same null and throws again, so #rdstations is never built and the state/title/buffer never update. machCardPoll (:3303) still fires it every 1 s. Play-by-URL, Save and Stop are separate functions and still work, so the card is not entirely dead.

- Fix: Delete the two #rdnote lines (the card is only shown while Radio is active, so the 'switch to Radio' note has no job), leaving .catch(function(){}) as the other pollers do. _[web]_

**#42 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/html/index.html:3241`** — edState() throws on missing #ednote; Editor card never builds its op buttons

- Why: #edcard (lines 802-816) has edsel/edops/edrange/edstate/edout/edbar/edm but no ednote (removed in 89be785). The .then throws on its first line and the .catch at :3259 throws on the same null, so #edops is never populated and state, progress, range and 'created X' never show. edLoad() still fills the file select, but with no op buttons edApply() cannot be reached from the page.

- Fix: Delete the two #ednote lines, same as the Radio fix; the card only exists while Editor is active. _[web]_

**#40 CONFIRMED · display · normal-use · `components/rest-api/html/index.html:2575`** — webPoll() throws on missing #ptag, so the Poll row never shows PAUSED

- Why: No element with id="ptag" exists; only the .ptag CSS rule at line 87 survives (the element went with the header in 499b75e, 09-10). webPaused flips first, so pausing itself works (pollStatus :3724 and sysPoll :2515 both return), but the TypeError skips webDraw() and the catch-up pollStatus(). The row keeps reading LIVE until some other webDraw() runs, and no paused tag ever appears. index.html was excluded from the 09-16 review, so this is not recorded there.

- Fix: Either restore a <span class="ptag" id="ptag" hidden>PAUSED</span> somewhere always visible (the CSS rule is still there), or null-guard the lookup the way webDraw() does with #webslot. Keep webDraw() and the resume pollStatus() after it. _[web]_


### L. Tape data loss

**#97 CONFIRMED · data-loss · normal-use · `components/machine_tape/tape.c:722`** — tape_cut deletes the full crop when the clipboard alloc was trimmed

- Why: bank_alloc returns 1 for a trimmed allocation (:81) but tape_copy only tests < 0 (:713), shrinks n to clip.cap (:714) and returns 0; tape_cut then recomputes n = out_pt - in_pt (:723) and closes the gap for the whole crop. Arithmetic: a 40 s tape is 3.53 MB of the 4.00 MB pool (tape.c:29-33), leaving at most 3 clip banks (about 4.4 s); 30 s plus delay and reverb slabs leaves about the same, so any longer Cut loses the remainder with no message. If zero banks are obtained the path is safe (-2), so only the partial case loses audio.

- Fix: Have tape_copy() pass bank_alloc's 1 (trimmed) through as its return value, and make tape_cut() bail without touching the tape when the return is non-zero or tp.clip_len < out_pt - in_pt. The Setup row can show the refusal the way stopped_or() does. _[task-memory]_

**#100 CONFIRMED · data-loss · normal-use · `components/machine_tape/tape.c:749`** — In-place edits never mark the take dirty, so they are dropped on leave or reboot

- Why: tape_norm/reverse/fade/cut/paste (:669-754) never touch take_dirty, dirty_gen or restore_id. Since the 11.5 fix, save_task clears take_dirty after the autosave (:820-821), so tape_stop (:1035) skips the save and writes the pre-edit id to tapelast (:1040), and tape_restore_last reloads the unedited file while the header keeps its saved check (tape_menu.c:189). Before 11.5 a recorded take stayed dirty forever and its edits were saved on leave, so for recorded takes this is a side effect of that fix; for loaded samples it was always so (:653). Nothing in CLAUDE.md documents edits as session-only; the only way to keep them today is the Crop button.

- Fix: In each edit that actually changed the buffer set tp.take_dirty = true, bump tp.dirty_gen (so an in-flight save cannot mark it clean) and clear tp.restore_id[0] (drops the saved check). The existing tape_stop / tape_load_inner save path then persists the edited buffer as a new CUT_ take. _[other]_

**#102 CONFIRMED · data-loss · rare · `components/machine_tape/tape.c:400`** — Momentary rec: tape-full with gate still held restarts a fresh take over the one just finished

- Why: At tape-full (tape.c:514) tape_rec_stop() queues autosave_req and sets recording=false, then playing=false; the next block's momentary branch (400-402) sees gate && !recording && !playing and calls tape_begin_fresh(), which zeroes len and records from frame 0 with no release required. The writer is only spawned by tape_autosave_kick on the UI tick (300 ms fast / 1 s slow, tape_menu.c:719/738/1012), so up to ~13k frames are overwritten before save_busy is set and the drop_spoiled check at 560 cannot see them; after the spawn it does flag the file as a blend, but only as a log line. Result: the CUT_ file has the new audio at its head (not truncated, save_b stays at the old len), the buffer take is gone, and holding on repeats this every tape length. Not in the 09-16 review (11.6 is the rec_stop_target leftover at the same line, a different defect).

- Fix: Make the momentary punch-in need a fresh gate edge after a tape-full stop: set a latch in the tape-full branch at tape.c:514, test it in the `gate && !tp.recording` condition, and clear it when the gate is released (same shape as the existing tr2_recgest/tr2_armed handling). Clear rec_stop_target there too (11.6). _[audio-dsp]_

**#96 CONFIRMED · data-loss · rare · `components/machine_tape/tape.c:409`** — TR1 'plays' a blank tape; TR2 then starts an overdub that writes nothing

- Why: tape.c:390 lets TR1 set playing=true with len==0 when rec_src is INPUT (the default, and the Live hint says 'TR1:play TR2:rec' on a blank tape). TR2 (punch :409 or momentary :401) then takes tape_rec_start's overdub branch (:170, rec_extend=false), and the write plan at :515 needs w < tp.len, which is 0, so no frame is ever stored while the header shows REC. Punch-out via tape_rec_stop does nothing (no rec_extend, len 0), so nothing is saved; playing an empty tape also mutes the monitor (:483-485). Not in the 09-16 review; only the 0.7 s hold-erase gesture escapes it.

- Fix: In tape_rec_start() take the fresh-take branch when `!tp.playing || tp.len == 0` (covers punch and momentary), and/or drop the `|| tp.rec_src == TPS_INPUT` clause at :390 so TR1 does not roll an empty tape. _[audio-dsp]_

**#103 CONFIRMED · data-loss · needs-fault · `components/machine_tape/tape.c:902`** — Crop adopt runs after a failed or short save: playback stops, or a truncated loop replaces the tape

- Why: save_task's fopen/alloc failure path (782-787) and its short-write path (810-811, `ok` only gates take_dirty) leave adopt_id set and drop_spoiled false, so tape_drop_adopt_kick passes its only check (897), sets playing=false (901) and calls tape_load. With fopen failed, sample_resolve returns -2 and the loop just stops, resume not honoured, while the drop note stays green (tape_menu.c:462 only turns amber on drop_spoiled). With a short write the file is finalised at its truncated size, so tape_load_inner first writes the dirty take whole to the same full card (612, also short, and repoints tapelast), then overwrites the bank head with the partial loop and sets len and take_dirty=false (651-653). Not recorded on 09-16 (11.4 covers the load-back race, not save failure).

- Fix: Have save_task publish its result (a tp.save_ok flag, or clear tp.adopt_id on the fopen/alloc and !ok paths) and make tape_drop_adopt_kick return before `tp.playing = false` when the write failed, next to the existing drop_spoiled bail. On a tape_load failure restore `tp.playing = resume`, and let the drop note's amber `bad` state in tape_menu.c:462 cover a failed save as well. _[io-rest]_

**#95 CONFIRMED · wrong-behaviour · normal-use · `components/machine_tape/tape.c:187`** — rec_stop_target is never cleared by tape_rec_stop / tape_erase / a fresh punch-in

- Why: rec_stop_target is cleared only at tape.c:202 (tape_rec_stop_at) and :388 (TR1 stop); a second TR2 press falls through :214 to tape_rec_stop() (:226) and leaves it set. The finder's overdub case cannot fire (the cancelled take has len < target and an overdub keeps pos < len), but the next FRESH take without a TR1 stop in between (hold-erase at :423-427, one-shot end, tape-full) is auto-finalised at the stale target by :531-532, or its quantised punch-out stops immediately. New beyond 11.6 (Watch, tape-full/erase only): in MOMENTARY mode :404 calls tape_rec_stop_request() every block while the gate is released, so the block after the target is set it falls through to the immediate stop - Rec Quant never quantises in momentary mode and every take leaves a stale target.

- Fix: Clear tp.rec_stop_target in tape_rec_stop(), tape_erase() and the fresh branch of tape_rec_start(). In the momentary branch (:404) only call tape_rec_stop_request() when !tp.rec_stop_target, so a pending beat-stop is allowed to run to the target. _[audio-dsp]_

**#99 CONFIRMED · wrong-behaviour · rare · `components/machine_tape/tape.c:901`** — tape_drop_adopt_kick forces playing=false without checking tp.recording

- Why: The finder's overdub scenario is normally caught: overdub writes land inside [save_a,save_b), set drop_spoiled (:560) and the kick returns at :897. The guard misses a FRESH take: Crop with the window not at the tape start (save_a > 0), then TR2 from stop records from frame 0 and the short crop file closes before the head reaches save_a, so drop_spoiled stays false, :901 clears playing and tape_load refuses at :608. That leaves recording=true, playing=false, rec_extend=true with pos frozen: the head rewrites one frame (:511-513), the take stops growing under a REC header, and TR1 then restarts at pos 0 still recording over it. The same happens for any punch-in between save_busy clearing and the next menu tick; this pre-load window is not the one 11.4 closed (tp.loading only covers the load itself).

- Fix: In tape_drop_adopt_kick(), after clearing adopt_id, skip the adoption (log and return) when tp.recording is set, before touching tp.playing. To close the remaining gap, raise tp.loading and call machine_block_wait() before that check so the gates are already held. _[task-memory]_

**#101 CONFIRMED · wrong-behaviour · rare · `components/machine_tape/tape.c:760`** — Crop Beats with clock source OFF collapses the crop to 64 frames

- Why: With Clock Src OFF tape_beat_frames returns 0 (:88-90); tape_crop_beats has no b==0 guard, so out_pt = in_pt (:760-761) and crop_clamp (:575) pulls in_pt back 64 frames (or leaves 0/0, which tape_eff_window widens to 64). The Setup row calls it on every detent with no transport or clock check (tape_menu.c:855-857), so it also fires while playing, turning the loop into a ~689 Hz buzz. Only the crop points are lost, not audio; tape_rec_stop_request guards the same value with `beat >= 64` (:216), so the missing guard here is an oversight. Not in the 09-16 review.

- Fix: Return early from tape_crop_beats() when tape_beat_frames() is 0 (same `beat >= 64` test tape_rec_stop_request uses), and have tape_val case 0 show 'no clock' in that state. _[other]_


### A. Reader staging buffers unchecked

**#79 CONFIRMED · crash · needs-fault · `components/machine_deck/deck.c:124`** — Deck reader chunk/tail DMA buffers are never NULL-checked

- Why: reader_task allocates 16 KB + 1 KB MALLOC_CAP_DMA and uses both unchecked: chunk goes to sampfile_read at :204 and :247, tail at :213. deck_start only checks the PSRAM ring and worker_spawn (4 KB stack), so a start where 4 KB fits but a 16 KB block does not leaves a live reader with chunk NULL and the first track load writes through NULL. The 09-16 bench notes record the internal largest block dipping to 10 KB and a Deck start landing on Stub after a 22 KB leak, so the regime is real. Not recorded as a finding on 09-16 (6.10 was rest-api only).

- Fix: Allocate chunk and tail in deck_start next to the ring, where a failure already returns ESP_ERR_NO_MEM and the core falls back to Stub; hand them to the task and free them in deck_stop only after worker_stop() succeeds (leak otherwise, per worker.h). _[task-memory]_

**#89 CONFIRMED · crash · needs-fault · `components/machine_dualdeck/dualdeck.c:240`** — Reader task's two MALLOC_CAP_DMA buffers are never NULL-checked

- Why: chunk (16 KB) and tail (1 KB) go straight into reader_serve; sampfile_read (sampfile.c:29-35) passes dst to fread with no check, so a NULL chunk is written through on the first fill, which the autosaved ta/tb load triggers right after start. dualdeck_start has already returned ESP_OK because worker_spawn succeeded, so the Stub fallback never engages. It needs a 16 KB internal-DMA allocation to fail at task entry, which is an OOM condition I cannot establish as normal. deck.c:124-125 and sampler3.c:142 have the same unchecked pattern; not recorded on 09-16 (6.10 is rest-api only).

- Fix: Allocate chunk and tail in dualdeck_start() before worker_spawn (file-statics, as the rings are), return ESP_ERR_NO_MEM on failure so the core's Stub fallback takes over, and free them in dualdeck_stop() only after worker_stop succeeds. Apply the same to deck.c and sampler3.c. _[task-memory]_

**#57 CONFIRMED · crash · needs-fault · `components/machine_sampler3/sampler3.c:142`** — Reader's 16 KB DMA stage buffer is never NULL-checked

- Why: heap_caps_malloc(16384, MALLOC_CAP_DMA) at 142 is used unchecked by rebuild_head, the lsc build and the ring top-up; sampfile_read (sampfile.c:29-35) passes it straight to fread, so a NULL stage is a store to address 0 on the first load. s3_start cannot see the failure because the alloc happens inside the task. The 09-16 bench notes already record the internal largest block dipping to 10 KB, so a failed 16 KB contiguous alloc is a realistic low-RAM case. Not recorded on 09-16 (6.10 covers rest-api mallocs only).

- Fix: Allocate stage in s3_start before worker_spawn and hand it to the reader as the task arg; on NULL take the existing free-all path and return ESP_ERR_NO_MEM so machine_activate's Stub fallback applies. Free it after worker_stop succeeds (leak on timeout, same rule as the PSRAM buffers). _[task-memory]_

**#65 CONFIRMED · crash · needs-fault · `components/machine_slicer/slicer.c:219`** — Slicer reader's 16 KB DMA staging buffer is not NULL-checked

- Why: heap_caps_malloc(16384, MALLOC_CAP_DMA) at slicer.c:219 is used unchecked; slicer_start auto-loads the first sample (slicer.c:423), so scan_file calls sampfile_read(f, sf, NULL, 4096) at slicer.c:164, which is fread into address 0 (sampfile.c:32) and panics. The 09-16 triage itself records the largest internal block dipping to 10 KB, so a failing 16 KB internal alloc is a realistic low-RAM state. Deck (deck.c:124) and Sampler3 (sampler3.c:142) have the same unchecked pattern; none of these is recorded in the 09-16 review (6.10 was rest-api only).

- Fix: Allocate stage in slicer_start beside the PSRAM slabs and fail with ESP_ERR_NO_MEM (the Stub fallback from 1.1/1.2 then takes over); pass it as the worker_spawn arg and free it in slicer_stop only after worker_stop succeeds. Same treatment fits deck.c:124 and sampler3.c:142. _[task-memory]_


### B. Reader busy-spin on a zero read

**#61 CONFIRMED · hang · needs-fault · `components/machine_sampler3/sampler3.c:327`** — Loop-start cache build that reads 0 frames retries every loop with no delay (busy-spin)

- Why: When the first read returns 0 the block leaves lsc_valid false, sets lsc_start = lw and still sets worked = true (332-335), so the vTaskDelay(1) at 396 is skipped and the same fseek/read is retried immediately, forever, at priority 6. The ring top-up next to it only sets worked when got > 0, so this is the one path that breaks the 'sleep at least one tick' rule. The truncated-file trigger in the scenario is wrong: sampfile probe clamps frames to the real file size (sampfile_int.h:66-68, 123-124, 155), so it needs a card read error or removal while a long loop with start past the head is loaded. Task WDT does not watch idle tasks here, so no reboot: httpd (prio 5) and lower tasks are starved on that core until the machine is switched.

- Fix: Set worked only when got > 0 in the lsc build (as the ring path at 376-385 does), so a failed build falls through to the loop's vTaskDelay(1). _[task-memory]_

**#78 CONFIRMED · hang · needs-fault · `components/machine_deck/deck.c:233`** — Deck reader busy-spins without a tick when sampfile_read returns 0

- Why: With got==0 neither wpos nor cur_ff advance and the fill branch still ends in `continue` (deck.c:224-233), skipping vTaskDelay(1) at :260, so the prio-6 unpinned reader spins on sd_lock/fread; FatFS keeps fp->err sticky so every later read fails at once. The truncated-WAV trigger is wrong: frames are clamped to the real file size at probe (sampfile_int.h:66-68, :123-124, RAW = fsize/4), so only a card read error or pulled card gets here. httpd is prio 5 pinned to core 0 (rest-api.c:2687) and can starve; the deck stays silent until a new track load or machine switch (run flag is still checked). Not in the 09-16 review.

- Fix: On got==0 do not `continue`: treat it as end of data (reader owns file_frames, so set dk.file_frames = ff and clear dk.loading, log once) and fall through to the existing vTaskDelay(1). That ends playback cleanly through the existing EOF path instead of spinning. _[io-rest]_


### C. UI task self-block on machine switch

**#27 CONFIRMED · hang · rare · `components/menu/menu.c:746`** — menuBindMachineUI blocks forever when called on the UI task with a full event queue

- Why: menuSwitchMachine (menu.c:292/296) runs inside menuProcessEvent on ui_ev_loop, the queue's only consumer, and posts EV_MACHINE_BIND with portMAX_DELAY to the 64-slot queue (ui.c:325). While it is inside autosave_now + machine_activate + configSetStringSetting nothing drains: the two timer tasks add 4.3 events/s (ui.c:270/281), the encoder ISR adds one per detent (gpio.c:55-82), REST adds more. Radio stop waits up to 7 s (radio.c:367) and Tape stop waits on a take save with no bound (tape.c:1034-1037), so ~15 s alone, or a slow stop plus ~35 encoder detents, fills it; the UI task then sleeps forever holding disp_lock, no watchdog. Not in the 09-16 review.

- Fix: s_bind_pending is already the truth: make the send non-blocking (timeout 0) and have menuProcessEvent run menuMachineBindNow() on the first event it sees with s_bind_pending set, so the queued EV_MACHINE_BIND is only a wake-up. app_main's call keeps working unchanged. _[task-memory]_


### D. FX rack stale rows / init order

**#9 CONFIRMED · crash · rare · `components/fxrack/fxrack.c:339`** — fxrack_menu_val/adj use stale cached row params after a preset load changes the slot kind

- Why: The mechanism is a stale cache, not a cross-task race: POST /remote/params is queued as EV_REMOTE_PRESET and preset_load runs on the UI task (menu.c:618-630), which returns with no page re-entry. The hosts rebuild their rows only on EV_ENTERED_MENU or a param<0 adjust (e.g. synth_menu.c:675,690), so s_*fx_n and s_*fx_param keep the old kind's rows. The next encoder event on the FX page calls setup_menu draw, then render, then fxrack_menu_val with the old param: for Off, gen_desc[FXK_OFF].p is NULL, so even param 0 loads from address 8 and panics; for a kind with fewer params it reads past the table and calls whatever pointer is there. fxrack_menu_adj at line 363 has the same problem when the row is in edit mode.

- Fix: In fxrack_menu_val and fxrack_menu_adj return early when param >= gen_desc[k].np (np is 0 for Off, so this also covers p == NULL). In menu.c EV_REMOTE_PRESET, queue EV_ENTERED_MENU after preset_load, as EV_REMOTE_SETUP already does, so the FX page rebuilds its rows. _[other]_

**#12 PLAUSIBLE · crash · rare · `components/util/fxdelay.c:30`** — fxdelay_init/flanger_init publish bufL before cap and bufR while the slot is already live

- Why: fxrack_load stores rk->slot[s] (line 420) before calling init (lines 432-433), unlike slot_set, which inits first. It runs on the UI task on core 0 (EV_REMOTE_PRESET, patch load, machine bind) while the audio task on core 1 is already running process(), and the only guard is `if (rk->dly->bufL)`. In the window the delay sees cap 0 and bufR NULL, so r is -1 and bufR[-1] faults; the flanger captures cap 0 and `while (rp < 0) rp += cap` never exits, and with TASK_WDT_PANIC and the CPU1 idle check both off that is a silent audio hang, not a reboot. What is missing is evidence the window is ever hit: it is a few instructions wide (tens of ns per 1.45 ms block) unless the UI task is interrupted exactly there.

- Fix: In fxrack_load keep the parsed kind in a local, run fxdelay_init/flanger_init first and store rk->slot[s] only afterwards (slot_set's order, fxrack.c:303-310). In both init functions also set cap, bufR and len before storing bufL, so bufL is the last field published. _[task-memory]_


### E. Tape buffer freed under the save task

**#98 CONFIRMED · crash · rare · `components/machine_tape/tape.c:580`** — tape_set_len_sel and the buffer edits run while save_task is still reading the banks

- Why: tape_set_len_sel (:580), tape_norm/reverse/fade/cut/paste/clear and tape_load_inner (:608, waits only when take_dirty) gate on tp_stopped() alone, while tape_stop (:1034) is the only place that waits for save_busy before bank_free. Setup > Tape Len (tape_menu.c:870) or a preset `lsel` (:1079) during the multi-second autosave runs bank_free, which frees and NULLs blk[] under save_task's tp_rd loop (:793); shrinking 30 s to 15 s leaves the upper blk[] NULL for good, so the writer dereferences NULL (LoadProhibited), otherwise the file gets freed or reallocated memory. The edit ops instead produce a blended file that drop_spoiled (audio-task only, :560) never flags and that save_task then marks clean (:820). Distinct from 11.3, which fixed the audio-task side of the same free with machine_block_wait.

- Fix: In tape_set_len_sel() wait for the writer exactly as tape_stop does (`while (tp.save_busy) vTaskDelay(pdMS_TO_TICKS(20));`, also clearing autosave_req) before bank_alloc. Make the destructive edits and tape_load_inner refuse (or wait) while tp.save_busy, with the Setup rows showing 'saving...' as row 19 already does. _[task-memory]_


### F. Looper bounce and save

**#106 CONFIRMED · crash · rare · `components/machine_looper/looper.c:401`** — Bounce re-reads volatile len between the zero check and the modulo

- Why: lp_track_t.len is volatile (looper_priv.h:26), so line 400's t->len==0 test and line 401's j % t->len are two separate loads. The bounce blocks the UI task, but a TR1 edge is handled in the audio task: on a selected STOP track apply_cmd sets len=0 (looper.c:131, or :52). If that lands between the two loads the remainder traps (IntegerDivideByZero panic); the window is a few instructions per iteration over up to 352800x4 iterations. cmd_clear is dead code (no writer), so only the TR1 re-arm path is real; TR1 is a button unless it is the clock source (core default is CV4).

- Fix: Snapshot each contributing track's len (and eligibility) into locals in the gather loop at 380-386 and use only the snapshots in the mix loop. Better, share the busy flag from #1 so apply_cmd/TR ignore actions for the duration, which also stops a re-arm recording over a source being read. _[task-memory]_

**#104 CONFIRMED · corruption · rare · `components/machine_looper/looper.c:316`** — looper_save_track writes the WAV with no sd_lock

- Why: machine_looper never includes or takes sd_lock: mkdir (309), fopen (316), sampwav_start, every fwrite (333), sampwav_finish and fclose (337-338) run unlocked; only sample_next_index and writeJSONFile lock internally. CLAUDE.md says every SD burst must hold it, and the sibling Tape save does (tape.c:778-808). No reader task runs under Looper, so the collision needs other SD traffic during the save: a web upload or /files listing (raw f_open/f_opendir in httpd, rest-api.c:227/1389) during a panel save, or the /bounce recording writer or import task during either. I did not trace actual corruption (FatFS has FF_FS_REENTRANT=1); the rule violation itself is certain. Max file is 1.4 MB, not 2.8 MB.

- Fix: Mirror tape.c:778-808: sd_lock_take/give around mkdir+fopen+sampwav_start, around each 512-frame fwrite, and around sampwav_finish+fclose. Check the fwrite return while there (card full currently reports SAVED). _[io-rest]_

**#110 CONFIRMED · data-loss · needs-fault · `components/machine_looper/looper.c:333`** — looper_save_track ignores write errors and reports SAVED for a truncated loop

- Why: The fwrite at 333, sampwav_start (318), sampwav_finish (337) and fclose (338) results are all dropped and the function returns 0 unconditionally (360). The menu then shows SAVED (looper_menu.c:387) and /looper/save returns 200 (looper_web.c:66-78). On a full card sampwav_finish patches the header to the truncated size, so the file probes as a valid shorter WAV; writeJSONFile is void (fileio.c:311), so a missing sidecar is also silent. The e327f6f fix for 6.8/13.8 covered Editor, upload and Freesound but not this writer; the looper save was not a 09-16 finding. The function also does its fopen/fwrite/fclose without sd_lock.

- Fix: Follow the Editor pattern from e327f6f (editor.c:99, 283): set an io_err flag when fwrite != nfr or sampwav_start/sampwav_finish/fclose fail, break out of the loop, remove() the partial file and return a distinct negative code. Show FAILED instead of EMPTY on the menu row for that code; the web handler already reports any non-zero rc. Wrap the write bursts in sd_lock_take/give while there. _[io-rest]_

**#105 CONFIRMED · data-loss · rare · `components/machine_looper/looper.c:326`** — Save reads a track buffer the audio task can re-record during the write

- Why: len is captured once at line 326 and state is never rechecked through the multi-second write. The panel save blocks the UI task, but TR1 is handled in the audio task (looper.c:158 -> apply_cmd): a TR1 edge on a STOP track (or two on a PLAY track) re-arms and records over buf[0..]; a web save of trk=N is also open to panel encoder presses. Reads stay in bounds (captured len <= LP_BUF_FRAMES), so no crash; the saved WAV is old loop plus new input. The sidecar carries no length, so that part of the claim is wrong; cmd_clear is never set anywhere.

- Fix: Add one volatile per-track busy flag in lp_state_t that apply_cmd and the TR handlers honour (ignore action on a track being saved or bounced); save sets it, calls machine_block_wait(), writes, clears. Alternatively recheck t->len/state per chunk and abort + remove() the partial file, returning the existing -2 'STOP REC' style code. _[task-memory]_


### G. MP3 import

**#13 CONFIRMED · corruption · rare · `components/util/mp3.c:296`** — Freesound convert and import scan can run MP3 decodes at once on the shared global `input` buffer

- Why: This is 09-16 finding 3.4, which sits in themes G/H but in no fix tier and not in Watch; it was never fixed (samp_import_file, sampimport.c:300, still ignores samp_import_busy and fs_machine.c:353 calls it directly). The same-directory half of the claim is wrong: CONFIG_FATFS_FS_LOCK=5 makes the second open of usr/IMPMP3.TMP / IMPNEW.TMP fail, so the scan cannot double-convert or destroy the Freesound mp3 (one side just reports 'decode failed'). The real path is two decodes in different folders (Freesound in usr/, the scan on an MP3 in usr/DRUMS, KEYS etc.): both reach `input = malloc` (mp3.c:296), one block leaks, both decode through the other, then use-after-free and a double free. Both tasks are prio 5 and unpinned, and every web upload starts a scan.

- Fix: Make `input` and `readPtr` locals of decode() so the decoder holds no shared state. Have one import slot cover both callers: fs_pipeline claims samp_import_busy (or a static mutex inside samp_import_file) before it creates usr/<name>.mp3, and samp_import_start refuses while it is held. _[task-memory]_

**#14 PLAUSIBLE · crash · needs-fault · `components/util/mp3.c:116`** — Sync search scans 4096 bytes after a short first read; offset past nRead wraps the memmove length

- Why: mp3.c:100 calls MP3FindSyncWord(input, MAX_FRAME_SIZE) whatever nRead was, and `input` is an uninitialised malloc (heap poisoning is off, so freed contents survive; any FF FF pair matches the sync mask). If the first read is short and has no sync in the valid part, an offset > nRead makes the unsigned `nRead - offset` at :116 wrap and memmove panics. It needs a file that starts with 'ID3' (so magic_is_mp3 passes) with under 4 KB and no frame after the tag, plus stale bytes that look like a sync; the heap contents are what I could not establish. The memmove was added by the 3.5 fix (38239db), so this is a regression from it; later reads are safe because the stale tail was already scanned clean.

- Fix: Pass nRead to MP3FindSyncWord at mp3.c:100 and treat offset >= nRead as not found. _[io-rest]_

**#17 CONFIRMED · data-loss · needs-fault · `components/util/mp3.c:207`** — MP3 decode stops at the first frame error and still reports success, so a truncated import replaces the source

- Why: The loop ends on any MP3Decode error and never resyncs (mp3.c:164, :207), and decodeMP3FileSync returns 0 whenever any output exists (:312). samp_import_file then converts the short IMPMP3.TMP and imp_publish removes the original (sampimport.c:280), so an MP3 with one undecodable region mid-file becomes a shortened WAV with no error. The f_write at :198 is also unchecked, though a full card mostly fails safe later because convert_pcm checks its own fwrite (sampimport.c:248). Same class as 13.3/13.8, but this path was not in the 09-16 review and e327f6f did not touch mp3.c; it needs a damaged MP3.

- Fix: On a non-underflow error, resync with MP3FindSyncWord and continue, as radio.c:312-318 does; only INDATA_UNDERFLOW with nothing left to read ends the file. Have decode() return a status (check f_write's result and bw) so decodeMP3FileSync fails when the input was not consumed to the end. _[io-rest]_


### H. Unclamped values from preset / sidecar / config

**#118 CONFIRMED · hang · rare · `components/machine_granular/granular.c:175`** — Granular preset load takes density unclamped; spawn loop has no per-sample cap

- Why: granular_preset_load (granular.c:254-257) stores valueint with no range check, while the panel clamps density to 1..120 (granular_menu.c:133); the web machine form is a free number input (index.html:1737) that reaches it via POST /remote/params -> EV_REMOTE_PRESET -> preset_load, then autosaves. At INT_MAX that is ~48,700 spawn_grain() calls per sample (~1.5M per block, with soft-double math), so the audio task (prio 23, pinned core 1) effectively stops producing blocks; roughly 1e6 and up already overruns the 1.45 ms budget. The finder's watchdog claim is wrong: idle-task WDT checks and WDT panic are off (sdkconfig:417-420), so there is no reset, just dead audio that returns on every boot until K8 or the Setup Density row (which re-clamps) is touched. Negative density is also accepted and drives spawn_phase negative; a large one sticks through float absorption until the machine restarts.

- Fix: Clamp in granular_preset_load to the ranges granular_menu.c:132-135 already uses (grain_ms 10..500, density 1..120, spray/spread 0..100). In process(), floor spawn_phase at 0 so a bad value cannot leave a standing deficit. _[other]_

**#84 CONFIRMED · crash · needs-fault · `components/machine_deck/deck.c:362`** — deck_seek_beats divides by beat_tf with no zero guard

- Why: Every other user guards beat_tf (:94, :490, :586, :888, dk_len_for :444); this one does not, and the int64 divide by zero traps in the UI task on EV_FWD/EV_BWD when not locked+synced (deck_menu.c:346/352). The finder's threshold is wrong: beat_tf = 2646000/bpm is zero only above 2,646,000 BPM (1,323,000 with feel x2), so "bpm": 200000 gives beat_tf 13 and no crash. The only unclamped source is the sidecar at :292-293; analysis is bounded and the Setup row clamps 40..240 (deck_menu.c:418-419). So it needs an absurd hand-edited or corrupt sidecar.

- Fix: Clamp at the source in deck_load_track: treat a sidecar bpm outside a sane range (say 20..1000) as 0 = unknown, which also lets auto-analysis repair it. Add `if (!beat_tf) beat_tf = DK_RATE;` in deck_seek_beats to match the other callers. _[io-rest]_

**#94 CONFIRMED · wrong-audio · needs-fault · `components/machine_dualdeck/dualdeck.c:755`** — deck_rate has no beat_tf==0 guard; a huge sidecar bpm makes rate_sm NaN until restart

- Why: The sidecar bpm is only tested `> 20` (L393, L754); above about 2.65e6 (or inf) beat_tf is 0, seg_tf is 0, fmodf(x,0)/0 is NaN, and the clamps at L773-782 do not catch NaN, so rate_sm (reset only in start(), L810) stays NaN. rpos_f and s_last_out then go NaN, and NaN*gain poisons the mix for both decks before the float-to-int cast at L1258, until the machine restarts. Every writer of "bpm" in the tree (analysis, sampler3, looper) produces sane values, so it needs a hand-edited or corrupt sidecar and a locked clock. The loop and resync paths already guard `!beat_tf` (L613, L675, L990); not recorded on 09-16.

- Fix: Reject the value at the source: in dualdeck_load_track accept the sidecar bpm only inside a sane range (e.g. 20..1000), else leave track_bpm 0 so the track is re-analysed. Also add `&& beat_tf` to the lock condition in deck_rate, matching the guards at L613/L675. _[audio-dsp]_

**#31 CONFIRMED · crash · rare · `components/menu/menu_config.c:29`** — wifiSettingsChanged pairs file and in-memory settings by string-count index; strcmp on NULL valuestring

- Why: i advances only on string elements of the file but indexes every element of curSettings, and val->valuestring goes to strcmp unchecked (line 32). With settings such as {"encres":4,"ssid":..}, ssid pairs with cur[0], a number, so strcmp(.., NULL) panics the UI task on every long-press out of Settings (menu.c:472), before anything is saved; a string before it gives a spurious reconnect instead. It does not fire on firmware-written configs: createConfigFile (fileio.c:49-52) puts ssid and passwd at 0 and 1, and every writer replaces in place or appends. It needs a CONFIG.JSN that was hand-edited or re-sorted by a tool with a numeric key ahead of ssid/passwd, which validateConfig accepts because it checks by name only.

- Fix: Drop the positional loop and compare by key: cJSON_GetObjectItemCaseSensitive(..., "ssid"/"passwd") on both objects, require cJSON_IsString on both, then strcmp. This is the same shape as the wifiChanged check in settings_post_handler (rest-api.c:1098-1104). _[io-rest]_

**#117 CONFIRMED · crash · needs-fault · `components/machine_tracker/tracker.c:300`** — xmp_create_context() NULL result unchecked in render_task

- Why: xmp_create_context returns NULL on calloc failure (libxmp/src/control.c:36-39) and s_ctx is never checked. A NULL s_ctx is dereferenced by xmp_free_context (reads ctx->state) at line 604 on every tracker_stop, and by xmp_load_module_from_memory (load.c:427) on a load. The finder's trigger is off: the calloc is PSRAM-first with internal fallback (xmp_mem.c), so it needs PSRAM nearly empty right after the 352 KB ring allocation and no internal block big enough. In that state the file-buffer alloc usually fails first, so the certain crash is on leaving Tracker.

- Fix: On NULL set trk.fail_why ("no RAM for player") and trk.state = TRK_FAIL, then stay in the s_rd.run loop answering load_req with TRK_FAIL so worker_stop still works. Guard the xmp_free_context call at exit with `if (s_ctx)`. _[task-memory]_

**#147 PLAUSIBLE · corruption · rare · `components/machine_slicer/slicer_ot.c:137`** — slicer_build_ot bounds its loop on volatile sl.n_slices re-read after the <=64 check

- Why: The defect is in the code: n_slices is volatile (slicer_priv.h:72), checked once at 115 and re-read every iteration at 137, while the reader task (unpinned, prio 6, above httpd's 5) can store up to 128 from recompute_grid/pick_transients (slicer.c:37, 88). At 128 the loop writes to offset 58 + 127*12 + 12 = 1594 of the 832-byte buf on the httpd stack (slicer_web.c:41). What I could not establish is a realistic trigger: the store has to land inside a loop lasting a few microseconds, and reslices come only from panel edits or a load, so a panel detent must coincide with a web .OT download to that precision. The handler also ignores sl.loading, so a download during a load or reslice can return a half-updated slice map, which is far more likely than the overrun.

- Fix: Read sl.n_slices once into a local at the top of slicer_build_ot and use it for the range check, the loop bound and the count field. Have slicer_ot_get_handler refuse (409 or 503) while sl.loading is set. _[io-rest]_


### J. JSON files rewritten unsafely

**#20 CONFIRMED · data-loss · rare · `components/util/fileio.c:319`** — writeJSONFile truncates in place with no write check; a cut mid-save empties CONFIG.JSN or AUTOSAVE.JSN

- Why: fopen "wb" zeroes the file on disk at open, fwrite and fclose are unchecked and the function is void, so preset_store_save returns 0 whatever happened (preset_store.c:63-67). A power cut between open and close leaves a zero-length file: for AUTOSAVE.JSN the next save mints a new object and every other machine's state is gone (menu.c:707-713); a truncated non-empty one fails to parse and then every later save is skipped for good. An empty CONFIG.JSN fails validateConfig and is regenerated with placeholder WiFi credentials, losing encres/encdir (fileio.c:138-141). The window is tens of ms per save, 2 s after any edit; not recorded on 09-16.

- Fix: Write to a sibling temp file, check fwrite and fclose, then swap it in with the backup-rename pattern of imp_publish (sampimport.c:283-297), since FAT cannot rename over a file. Return int and let preset_store_save and the autosave log pass the failure on; on read, fall back to the backup when the main file is missing or empty. _[io-rest]_

**#24 CONFIRMED · data-loss · needs-fault · `components/menu/menu.c:914`** — Failed AUTOSAVE.JSN read on bind loads defaults, next autosave overwrites the saved entry

- Why: menuMachineBindNow treats a NULL from readJSONFileAsCJSON (DMA alloc of the ~5 KB file at fileio.c:245, short fread, parse fail) exactly like a missing entry and calls preset_load(NULL). s_bind_pending is already false, so the next encoder event or dirty poll arms autosave_now (menu.c:688), whose read now succeeds and replaces this machine's key with the defaults. autosave_now has the 'existing file but NULL read' guard (menu.c:707-712); the load side has none. 5b4ea8e moved cJSON nodes to PSRAM so the parse-OOM trigger is rarer, but the internal DMA buffer alloc and card errors remain.

- Fix: In menuMachineBindNow, when root is NULL and fileExists("/sdcard/AUTOSAVE.JSN") > 2, retry the read once; if it still fails set a static 'load failed' flag that autosave_now checks (next to the s_bind_pending check) and skips with a log until a later bind loads successfully. Still call preset_load(NULL) so the machine runs. _[io-rest]_

**#18 PLAUSIBLE · wrong-behaviour · rare · `components/util/fileio.c:205`** — getFileSize returns uninitialised stack data when the file is missing

- Why: The stat result is ignored and `st` is uninitialised; IDF 4.3 vfs_fat_stat returns -1 before its memset (vfs_fat.c:597-603), so st_size is whatever was on the stack. The only caller is autosave_now (menu.c:708), reached when AUTOSAVE.JSN is missing; if the garbage is > 2 every save logs 'read failed on existing file' and the file is never created. Whether that slot holds a value > 2 depends on the build's stack layout, which I could not establish from reading. The stat also runs without sd_lock.

- Fix: Use the existing fileExists() (fileio.c:209, zero-inits and checks the return) at menu.c:708, or make getFileSize return 0 when stat fails, under sd_lock. _[io-rest]_

**#29 PLAUSIBLE · data-loss · rare · `components/menu/menu_config.c:62`** — CONFIG.JSN read-modify-write is not locked across the cycle (UI task vs httpd)

- Why: configSetStringSetting/configSetIntSetting, the Settings exit merge (menu.c:499-513) and three REST writers (rest-api.c:1229, 1826, 2317) each read, modify and rewrite the whole file with sd_lock held only inside the read and inside the write. Writers run on two tasks: UI (machine, clk_*, tapelast; the Tape calls at tape.c:48/1040 are UI context, not a third task) and httpd. If the prio-11 UI task completes a cycle between httpd's read and write, the UI's key is rolled back; a NULL read drops the write silently. The trigger needs a web settings write and a panel/remote-machine write within the same ~100 ms, which I could not show happens in an ordinary flow. 09-16 finding 4.2 fixed the page-lifetime snapshot rollback, not this.

- Fix: sd_lock is recursive: take it around the whole read-modify-write in one shared helper in menu_config.c and route configSet*, the Settings-exit merge and the three rest-api.c writers through it. Log when the read returns NULL. _[io-rest]_

**#86 CONFIRMED · data-loss · rare · `components/machine_deck/deck_analysis.c:54`** — Deck sidecar read-modify-write is not atomic between analysis commit and deck_set_feel

- Why: readJSONFileAsCJSON and writeJSONFile each take sd_lock on their own (fileio.c:225, 318); nothing holds it across the read-modify-write in deck_analysis_commit (deck_analysis.c:54-69, analysis task prio 4) or deck_set_feel (deck.c:386-392, UI task prio 11, from deck_menu.c:434). If the Feel edit arrives while commit is in or just past its read, the UI task blocks on sd_lock, wakes when the read releases it, writes {old + feel}, then commit writes its stale root and drops "feel". The window is tens of ms once per analysis, so only that direction is realistic; the lost-bpm order would need the analysis task to fit two lock acquisitions into the UI task's microsecond gap. Loss is the per-track feel reverting to x1 on the next load; not in the 09-16 review.

- Fix: Hold sd_lock (recursive) across the whole read-modify-write in both deck_analysis_commit and deck_set_feel: sd_lock_take() before readJSONFileAsCJSON, sd_lock_give() after writeJSONFile. The nested takes inside the fileio helpers are fine with the recursive mutex. _[io-rest]_


### K. Settings not saved, or wiped by a partial update

**#167 CONFIRMED · data-loss · normal-use · `components/machine_slicer/slicer.c:665`** — Slicer preset_save omits the FX box, reverb mode and mix, and the OT/grid choice

- Why: slicer_preset_save/load (slicer.c:656-684) carry no FX keys, although fx_on is toggled on the Live page (slicer_menu.c:287) and reverb mode and mix are Setup rows 6/7 (slicer_menu.c:357-367). slicer_start memsets sl and sets Room at 25% with the box off (slicer.c:384, :404-409), so after a switch or reboot the state is that default, not 'Off / 0' as the finding says, but the user's settings are lost all the same. ot_active is not stored either and is re-derived from the .ot file on every load (slicer.c:244), so a Grid choice on a sample with an .OT reverts. fx_filt is a knob (matrix ABS entry) and correctly needs no key; nothing in CLAUDE.md or the slicer-fx-v1 README says this is deliberate.

- Fix: Add "fx" (bool), "rv" and "rvmx" (the fxrack key names) and an "ot" mode key to slicer_preset_save, and restore them in slicer_preset_load with reverb_set_mode / reverb_set_mix, range-checked against RV_N_MODES and 0..1. Apply the stored OT choice after the reader's load instead of forcing ot_active from the file. _[io-rest]_

**#168 CONFIRMED · data-loss · normal-use · `components/machine_drumsampler/drum.c:745`** — Drums preset omits knob-7 clockwise values (attack_ms, start_off, loop_ms)

- Why: process() sets attack_ms/start_off/loop_ms from knob 7 and flags machine_state_dirty() (drum.c:262-297), and the Pads page shows them (drum_menu.c:652-663), but drum_preset_save writes only dec/cw/reps/pit and drum_preset_load has no key for the three. drum_start memsets dr (drum.c:120), so after a reboot or machine switch they are 0 and return only when knob 7 is moved with that pad selected (takeover). CLAUDE.md states the intent that kits dialed in by knob autosave, and the sibling knob targets decay_ms and pitch_semi are saved. Not in the 09-16 review (chunk 9 reported nothing for Drums).

- Fix: In drum_preset_save add three per-pad keys next to "dec" (attack_ms, start_off, loop_ms); in drum_preset_load read them after "cw" with clamps to DR_ATTACK_MAX, DR_START_MAX and DR_LOOP_MIN_MS..DR_LOOP_MAX_MS (0 allowed). The existing cvmtx_rearm + knob_last=-1 already keeps the loaded values until the knob moves. _[other]_

**#169 CONFIRMED · data-loss · normal-use · `components/machine_granular/granular.c:244`** — Granular preset omits position, pitch and level, which became stored values with takeover

- Why: Since 15c1ead position/pitch_cv/level are written only when cvmtx_abs reports a live (moved) knob (granular.c:160-164); before that commit they were read straight from the knobs every block, so nothing needed saving. granular_preset_save/load (238-262) carry grain_ms/density/spray/spread/sample and the matrix only, and granular_start resets position=0, pitch_cv=2048, level=255. So after every boot or switch the cloud sits at the sample start at unity pitch regardless of the knob until it is moved past the 0.03 threshold, while cvmtx_track still flags the autosave for those moves. The load comment "knobs recapture against the loaded values" has no loaded value for these three; not in the 09-16 review.

- Fix: Add position, pitch_cv and level to granular_preset_save and read them clamped (0..4095, 0..4095, 0..255) in granular_preset_load before cvmtx_rearm. Alternatively mark them performance-only with gr.mtx.nodirty and seed from the assigned channel the way Tracker's loop-window seed does; saving matches what grain_ms/density already do. _[other]_

**#164 CONFIRMED · data-loss · rare · `components/machine_instsampler/instsampler.c:756`** — A zone or pad that fails to load is dropped from the next autosave (Keys and Drums)

- Why: keys_preset_load skips a failed keys_load_zone_at with `continue` (arena full, load failed, no PSRAM), and keys_preset_save (instsampler.c:657-659) only writes zones below nzones with frames != 0, so the next encoder touch or machine switch rewrites the Keys entry without that zone. A zone truncated to the remaining arena room is kept, but its loop_end is clamped at :771 and saved that way. Drums is the same: drum_load_layer blanks L->sample on alloc or read failure (drum.c:672, :677) and preset_save writes it as "s":"" (drum.c:735/743). The arena ladder landing short is a documented ordinary condition, not only a fault; named PAT_ patch files are not affected, only the autosave.

- Fix: Keys: keep the names (SAMPLE_ID_LEN) and root/fn/loop of zones that failed during preset_load in a small pending list that keys_preset_save appends to "zones", cleared by Load Sample / Clear Zones. Drums: on a failed load keep L->sample with len 0 so "s"/"s2" round-trip and the next boot retries. _[task-memory]_

**#162 CONFIRMED · data-loss · rare · `components/fxrack/fxrack.c:429`** — fxrack_load sets both FX slots Off when the node has no "fxsl" (partial /remote/params)

- Why: With no "fxsl" array the else branch at fxrack.c:423-430 runs the legacy migration, finds no legacy keys, sets both slots Off, and slot_gc at :482 frees the delay/flanger slabs; Keys, Synth, Tape and Drums all call it unconditionally (instsampler.c:800, synth.c:344, tape.c:1089, drum.c:772) and EV_REMOTE_PRESET then arms the autosave. The web page always posts the full readback including fxsl, so only partial posts hit it: tools/bench/mix_sweep.py posts {"rvmx":..} alone and rig.py:256 documents the body as a partial preset. The 5.2 fix applied exactly this partial-update rule to cvmtx_load (cvmtx.c:287) but not to the rack.

- Fix: In fxrack_load, touch rk->slot[] only when "fxsl" is an array or at least one legacy key (od/flg/trem/dly) is present; otherwise leave the slots as they are, mirroring cvmtx_load's 'no matrix = partial update' rule. _[io-rest]_

**#166 CONFIRMED · data-loss · rare · `components/machine_tracker/tracker.c:919`** — tracker_preset_load resets file, flags and CV matrix before reading a partial node

- Why: tracker.c:914-920 set loop/sync/amiga/show_text/loop_freeze to defaults, zero trk.file and call cvmtx_reset_defaults before the `if (!node) return`, so a body without those keys wipes them; the running module keeps playing but the 2 s autosave stores file "" and the defaults. The web page posts the full readback so it is unaffected; partial posts do hit it (tools/bench/review_bench.py:361 posts {"file":..} alone and so resets sync and the matrix). Freesound is the same: load_str_array zeroes the count when the key is absent and autoplay falls back to true (fs_machine.c:796-798, :821-823). The 5.2 fix made cvmtx_load honour partial updates, but Tracker's unconditional reset bypasses it.

- Fix: Apply the defaults and cvmtx_reset_defaults only when node is NULL; otherwise overwrite only the keys present (cvmtx_load already resets when a stored matrix is present). In Freesound's load_str_array leave dst and the count untouched when the key is absent, and only change autoplay when the key exists. _[io-rest]_


### M. Recording service and OTA

**#37 CONFIRMED · data-loss · rare · `components/rest-api/rest-api.c:2469`** — POST /ota does not refuse while a recording is active

- Why: ota_post_handler has no recording_is_active() check and ends in esp_restart() at line 2522, while reboot_post_handler refuses with 409 at 2539 for exactly this reason. tools/ota.sh only reads /ota/state before posting, so nothing outside the firmware guards it either. rec_writer_task never syncs during a take (recording.c:166-198; sampwav_finish and fclose run only at the end), so a restart mid-take leaves an unclosed file with no sidecar and the take is lost. It needs a flash while a take or bounce is running.

- Fix: Put the same recording_is_active() 409 block that reboot_post_handler uses at the top of ota_post_handler, before esp_ota_begin(). Consider also refusing when recording_is_prepared(). _[io-rest]_

**#4 CONFIRMED · data-loss · rare · `components/audio/recording.c:111`** — Take index above 9999 sends every take to one <PFX>OVFL.WAV, truncated each time

- Why: hint > 9999 returns the fixed name without advancing, and the writer opens it with "wb" (recording.c:122), so each take destroys the previous one with no refusal or error. It is easier to reach than the finder said: sample_next_index() (sampfile.c:130-131) matches the prefix case-insensitively in every pool folder and takes atoi() of whatever follows, so with long filenames on, one uploaded file such as rec_20260913_jam.wav sets the counter to 20260914. In Sampler3 the overwritten file is also the one the voice auto-loaded and is streaming. The 8.3 limit that motivated the 4-digit cap no longer exists.

- Fix: Drop the OVFL branch: with LFN on, %s%04d prints five or more digits and still fits fname[48] and SAMPLE_ID_LEN. In sample_next_index() count only names whose digits run straight into the extension, so a date-stamped upload cannot set the counter. _[io-rest]_

**#1 CONFIRMED · wrong-behaviour · rare · `components/audio/recording.c:137`** — Writer parks on rec_active level; a stop during file setup strands it, bounce refused until reboot

- Why: The park loop tests the level of rec_active, which recording_stop()/recording_finish() (recording.c:302/284) clear; the writer first runs find_next_filename (readdir of all 7 pool folders on first use or prefix change) and fopen, so a start then stop inside that window leaves it parked with the file open and rec_prepared true. audio_bounce_start() then returns at audio.c:168 (recording_is_prepared()), so /bounce/start and the Settings Bounce toggle (menu.c:438) report busy until reboot. Finder overstated one point: a later recording_trigger() (Sampler3 armed gate, Tape card mode) also un-parks it, but into the stale BNC_ file with the old vid and any chunks still queued. Not in the 09-16 review (1.4 and 1.7 are other stuck-prepared paths).

- Fix: Add an atomic rec_triggered latch set in recording_trigger() and cleared in recording_prepare(); park on !rec_triggered && !rec_cancel. A writer that wakes with rec_active already false falls into the existing drain loop and finalises the short take, or with an empty queue takes the existing cancel branch (fclose + remove under sd_lock). _[task-memory]_

**#7 CONFIRMED · wrong-behaviour · needs-fault · `components/audio/recording.c:203`** — Failed or zero-chunk take leaves its header-only file in usr/REC

- Why: The failure branch at recording.c:203-207 only logs; only the parked-cancel path (143-144) removes the file. The finder's 'stopped before any block was queued' route does not occur with current callers (the writer either stays parked or at least one chunk is queued), so the real trigger is a write or close error on the first batch, such as a full card. The leftover matters slightly more than stated: sf_wav_parse() accepts a 44-byte WAV as valid with frames = 0 (sampfile_int.h:66-74), and the boot sweep covers only usr and usr/TRACKER (fileio.c:100-103), so the entry stays in the pool. Same class as 13.9 (Editor zero-frame file, Watch).

- Fix: When chunks_written == 0, remove(fname) inside the sd_lock section that already does sampwav_finish + fclose, as the cancel branch does. Keep the partial take when write_err occurs with chunks already written (the comment says that is deliberate). _[io-rest]_


### N. Import, upload and file handling

**#182 CONFIRMED · data-loss · normal-use · `components/rest-api/rest-api.c:2051`** — /trk/upload still clamps the module name to 8 chars and silently replaces a same-prefix module

- Why: `if (nl > 9) b[8] = 0;` cuts the Name header to 8 chars into name[16], then f_unlink(path) and f_rename publish over whatever already has that name (2113-2114). The page sends nameOf(b), up to 31 chars (index.html:1346, 1423), and counts each PUT as uploaded, so AMBIENT_PART1.xm and AMBIENT_PART2.xm both land as AMBIENT_.XM. Every module name over 8 chars is truncated; the overwrite needs two that share the first 8 chars and the extension. This is a long-filename leftover the 09-16 theme A sweep did not list; path[48] is also too small for a 31-char name (13+31+1+4+1 = 50).

- Fix: Follow drop_sample_put_handler and drop_ot_put_handler: refuse rather than truncate. Size name to SAMPLE_ID_LEN and path to 64, and return 400 when strlen(name)+1+strlen(ext) exceeds 31, because /trk/list and /trk/get only handle filenames under 32 chars (2209, 2232). Cap nameOf() for kind 'mod' in the page to match. _[io-rest]_

**#137 CONFIRMED · wrong-behaviour · normal-use · `components/util/sampimport.c:275`** — Re-upload under an existing name is refused by imp_publish and left unconverted

- Why: The web page uploads mp3/wav/aif bytes as-is (index.html:1292,1342,1417) to usr/<name>.RAW; the first upload converts to <name>.WAV, the second hits stat(dst)==0 at sampimport.c:274 and returns -1 while drop_sample has already answered 200. Finder's consequence is wrong in one detail: the old audio does not keep playing, because sample_resolve tries .RAW first (sampfile.c:72,88-91), so the unconverted upload shadows the old WAV: a 48k/24-bit WAV then fails to load ("WAV: not 44.1kHz") and MP3 bytes fall through sf_parse as headerless RAW (sampfile_int.h:151) and play as noise. Every later scan retries and fails again. The 3.3 fix holds for what it recorded (no source loss); this is the case it left refused. The Freesound same-id case only yields a misleading "convert failed" with the existing sample intact.

- Fix: In imp_publish, when dst exists and src is the upload/download scratch twin (.RAW or .MP3 of the same base), replace it with the existing IMPBAK.TMP park-and-rename path (dst to bak, tmp to dst, remove bak and src), restoring on failure. Keep the refusal for X.AIF beside X.WAV, and surface the failure through the import fail counter the page already polls. _[io-rest]_

**#36 CONFIRMED · wrong-audio · normal-use · `components/rest-api/rest-api.c:1478`** — Upload finishing during an import scan is never converted

- Why: samp_import_start() returns -1 while busy (sampimport.c:446) and there is no pending-rescan flag; the only other caller is POST /import. The web batch sends mp3/wav/aif as-is, back to back (index.html DEV_EXTS 'native'), so upload N+1 is open for write while upload N's scan runs. The scan's fopen of that file fails FR_LOCKED (FF_FS_LOCK=5) and it is skipped (sampimport.c:171-175, 352-354), or its dir slot is already behind the readdir cursor. If the upload then completes while the scan is still walking the other six folders, its own kick is dropped; an MP3 stays under .RAW, sampfile_probe falls back to headerless RAW (sampfile_int.h:151) and it plays as noise until a manual Rescan or a later upload.

- Fix: In sampimport.c add a volatile rescan-requested flag: samp_import_start() sets it and returns 0 when samp_import_busy is already true. import_task repeats the folder walk while the flag was set during the pass, and only then clears samp_import_busy. _[io-rest]_

**#124 CONFIRMED · data-loss · rare · `components/machine_freesound/fs_web.c:148`** — /fs/fetch onto a name held by X.RAW installs a shadowed X.WAV and overwrites X.JSN

- Why: fs_fetch_handler uses fs_safe_name with no collision check; imp_publish only stats the .WAV destination (sampimport.c:274), so with usr/X.RAW present (web uploads land as .RAW) usr/X.WAV publishes and fs_write_sidecar rewrites /sdcard/usr/X.JSN (fs_machine.c:150). sample_resolve tries .RAW first (sampfile.c:72), so the job reports 'installed' but id X still plays the old file with the new sidecar. Drop Last after that DONE then removes the user's X.RAW, so data-loss stands. Not recorded on 09-16; 3.3 and 13.4 fixed neighbouring cases.

- Fix: In fs_fetch_handler refuse with 409 'name taken' when sample_resolve(name) == 0, mirroring the test in fs_pool_name. No id exists to suffix on this path, so refusing is the simple option. _[io-rest]_

**#123 CONFIRMED · data-loss · rare · `components/machine_freesound/fs_menu.c:437`** — Setup > Drop Last deletes an existing pool sample after a failed or colliding /fs/fetch

- Why: start_job sets fsm.cur_name before the job runs (fs_machine.c:399) and neither set_err nor fsnd_start clears it; fs_fetch_handler only runs fs_safe_name (fs_web.c:148), with no sample_resolve check. With KICK.WAV in the pool, a fetch named KICK fails (imp_publish 'exists', sampimport.c:274, or any network error) and leaves cur_name=KICK; Drop Last then auditions the user's KICK successfully and fs_audition_drop removes its audio and .JSN. The 13.4 fix (fs_pool_name) covers the panel and /fs/get only, not this path.

- Fix: In fs_setup_action/fs_setup_val fall back to fsm.cur_name only when fsm.phase == FS_DONE (the job actually installed it). In fs_fetch_handler return 409 when sample_resolve(name) == 0, the same test fs_pool_name uses. _[io-rest]_

**#184 CONFIRMED · data-loss · rare · `components/rest-api/rest-api.c:348`** — Delete/rename/move sweep an id across all seven folders; same id in two folders is handled as one

- Why: files_delete_handler removes name+ext in every del_dirs entry (346-350), rename renames every match (464-471), and the page sends only `name` and filters or re-tags every local row with that name (index.html:1235, 1253, 1275). /files lists each folder on its own, so twins show as two rows. The sweep is deliberate for pieces of ONE sample, but id uniqueness is not enforced: drop_sample_put_handler opens /usr/<id>.RAW with FA_CREATE_ALWAYS and no cross-folder check (1362, 1389), so re-uploading a name that was moved to DRUMS/KEYS creates a twin, as does a card copy. The second twin is already shadowed for loading (sample_resolve takes the first folder, sampfile.c:88), but one delete click removes both files and their sidecars.

- Fix: Enforce the invariant where twins are created: in drop_sample_put_handler call sample_resolve(id) and refuse (or replace in place) when the id already lives in another folder, as files_rename_handler already does with 'Target name already exists'. Additionally accept an optional `dir` on delete/move/rename (mv_tags lookup already exists) to restrict the sweep, and have the page send f.dir and match rows on name+dir. _[io-rest]_

**#47 CONFIRMED · data-loss · rare · `components/rest-api/html/index.html:1346`** — Batch upload names can collide after sanitise/truncate; later file overwrites earlier

- Why: fileMeta strips to [a-zA-Z0-9_] and nameOf cuts to 31 or falls back to FILE; stageBatch and doBatchUpload never compare names within the batch or against libFiles. The server opens usr/<NAME>.RAW with FA_CREATE_ALWAYS (rest-api.c:1389), so a second PUT with the same name replaces the first and the summary still counts both as uploaded. The staging list does show the names, but line 1365 appends base.substring(8) in grey after the full 31-char name (an 8.3 leftover, as is maxlength=8 at :1359), which garbles long names and hides a collision. Replacing an existing library sample by re-uploading its name may be intended; the intra-batch loss is not.

- Fix: In stageBatch/renderBatch, detect repeated nameOf() values in the batch and names already in libFiles; mark those rows and either block Upload or suffix _2, _3 within 31 chars. Fix the leftovers in the same pass: maxlength 31 at :1359 and the grey tail at :1365 to substring(31). _[web]_

**#38 CONFIRMED · data-loss · needs-fault · `components/rest-api/rest-api.c:2172`** — PUT /drop_ot ignores f_write result, publishes short .OT

- Why: Incomplete fix of 6.8: mod_upload (2102-2109) and drop_sample (1424-1438) now check the write, but drop_ot at line 2172 still ignores both the FRESULT and bw, and the f_close result at 2175. On a full card or write error the handler still unlinks the existing <NAME>.OT, renames the short temp into place and returns 200. The finder overstates one part: slicer_parse_ot rejects a file that is not 832 bytes (slicer_ot.c:62), so no corrupt map is applied. The real loss is the old good slice map, with a false success reported.

- Fix: Copy the mod_upload_handler check: on wr != FR_OK or bw != ret, f_close, f_unlink(OT_TMP) and return 500 "SD write failed". Also check the f_close result before f_unlink(path) and f_rename. _[io-rest]_

**#138 PLAUSIBLE · data-loss · needs-fault · `components/rest-api/rest-api.c:1455`** — drop_sample ignores the f_close result (the pad write cannot fail on a full card)

- Why: The card-full scenario does not hold: the pad is written only when content_len % 4 != 0, so the file end is mid-sector in an already-allocated cluster and the 1-3 bytes are a copy into the FIL sector cache with no allocation and no disk I/O; a full card is caught by the checked loop write at rest-api.c:1426, so the 6.8 fix holds. What remains is the unchecked f_close at rest-api.c:1468: if its flush or directory update hits a card I/O error, the handler still returns 200 and starts the importer on a file whose directory entry was never updated (size 0). I could not establish a trigger other than a failing card.

- Fix: Check the FRESULT of the pad f_write and of f_close; on failure reuse the existing error branch at rest-api.c:1430-1437 (f_unlink, EV_DECODING_DONE, 500 "SD write failed") and skip samp_import_start(). _[io-rest]_

**#22 CONFIRMED · data-loss · needs-fault · `components/util/fileio.c:82`** — Boot sweep deletes IMPBAK.TMP, the in-place import backup; pool subfolders never swept

- Why: imp_publish (sampimport.c:285-296) parks X.WAV as <dir>/IMPBAK.TMP and then renames IMPNEW.TMP over it; a power cut between those two renames leaves the audio only in two *.TMP files, and sweepDir (fileio.c:82-96, run from checkSDStructure:144) removes both in usr/. The window is narrow: two back-to-back FAT renames under sd_lock, per converted file. sweepIncompleteFiles (fileio.c:100-103) covers only usr and usr/TRACKER while import_task scans seven folders (sampimport.c:401-404), so IMP*.TMP leftovers in REC/LOOPS/SLICES/DRUMS/KEYS/TAPE stay until the next conversion in that folder overwrites or removes them (not strictly forever), and nothing ever restores a stranded backup. The backup name came in with the 3.3 fix (e327f6f); 3.3 itself is fixed.

- Fix: Name the backup after the file it protects (e.g. X.WAV.BAK, not a .TMP and not a pool extension) so the sweep can put it back when X.WAV is missing and delete it otherwise. Run sweepDir over the same seven pool folders the importer scans, plus TRACKER. _[io-rest]_

**#52 CONFIRMED · data-loss · rare · `components/rest-api/html/index.html:1506`** — saveSet always posts ssid; firmware stores an empty ssid and restarts WiFi with it

- Why: g() returns '' when #gsSsid is absent, and the Save button is rendered by globCard (2435) even while rpGlob is null and the screen is an empty div (2430). glLoad's J() turns a failed /settings into {} (2354), so the SSID row then renders blank. settings_post_handler replaces ssid for any non-null string (rest-api.c:1097-1100), unlike the strlen guards on passwd (1102) and apikey (1110), sets wifiChanged and calls restartWifi with the empty SSID 500 ms later (1234-1238), so the unit drops off the LAN at once, not only on next boot. Recovery is less dire than claimed: the panel Settings page can re-enter the SSID and the 6.4 AP fallback exists.

- Fix: In settings_post_handler give ssid the same strlen(j->valuestring) > 0 guard that passwd and apikey have (rest-api.c:1102, 1110). In saveSet include only keys whose input element exists, and skip the POST when rpGlob is not loaded. _[io-rest]_

**#133 CONFIRMED · wrong-behaviour · needs-fault · `components/machine_freesound/fs_machine.c:153`** — Freesound sidecar write failure is ignored; job still reports done

- Why: writeJSONFile (fileio.c:311) returns void and ignores fopen/fwrite/fclose results; fs_write_sidecar returns 0 unconditionally, also when cJSON_Print returns NULL, so the check at :365 is dead. GET /files skips ids with no .JSN (rest-api.c:264), so the take is missing from the web list, and fs_pool_name then treats the bare WAV as a different sound and re-downloads it as <title>_<id>. Overstated parts: the panel browsers do not need the sidecar, Drop handles a missing one (:735-738), and a failed f_close at :324 leads to 'convert failed', not a false success.

- Fix: Make writeJSONFile return int (fopen, fwrite length and fclose checked; existing callers may ignore it) and have fs_write_sidecar return that, with -1 when cJSON_Print is NULL. The set_err("sidecar write failed") path at :365 already exists. _[io-rest]_


### Q. Web Apply sends something other than what the device holds

**#176 CONFIRMED · wrong-audio · normal-use · `components/rest-api/html/index.html:1764`** — FX3 dropdown lacks Shimmer; any Apply posts rv:0 and turns reverb off

- Why: rv_mode_t has RV_SHIMMER = 4 (reverb.h:23-28), the panel cycles through it (fxrack.c:346-350), and fxrack_save emits rv as the raw mode (fxrack.c:372). fxSel (1781-1785) over the 4-entry FXRV marks no option selected for 4, so the select's value is '0'. rpApply (2643-2649) harvests every [data-rp] in the document and posts rv:0, and fxrack_load (408-411) calls reverb_set_mode(RV_OFF). Applies to every fxrack host: Keys, Synth, Tape, Drums.

- Fix: Add 'Shimmer' to FXRV so it matches reverb_mode_name(). Optionally have fxSel emit a selected option for a cur value outside the list, so a future enum addition cannot be posted back as 0. _[web]_

**#178 CONFIRMED · wrong-audio · normal-use · `components/rest-api/html/index.html:2199`** — Drums pad level slider max 255 clamps driven pads (up to 1023) on Apply

- Why: Pad level is uint16 up to DR_LEVEL_MAX 1023 (drum_priv.h:162), set by knob 6 past noon (drum.c:253-260) or the panel Level row (drum_menu.c:731-735), and emitted raw as lvl (drum.c:736). A range input sanitises a value above max down to max, so inp.value reads 255 while the label span still shows the real number. rpApply posts lvl:255 and drum.c:807-811 stores it, so the pad drops to unity until the knob is moved again. Any Apply on the page does this, because rpApply harvests all [data-rp] inputs.

- Fix: Set the level slider's max to 1023 (DR_LEVEL_MAX) and note that 255 is unity in the label; leave pan at 255. _[web]_

**#177 CONFIRMED · wrong-behaviour · rare · `components/rest-api/html/index.html:2188`** — Drums 'trig in' select has no 'none'; Apply rewires src 0 pads to CV1

- Why: drum_preset_save emits src = trig_src + 1, so DR_SRC_NONE (-1, reachable via src_cycle at drum_menu.c:710-714) is 0 (drum.c:741). The select only builds options 1..8, none is selected for 0, the browser falls back to CV1, and rpApply posts src:1, which drum.c:826-828 maps to trig_src 0 = CV1. The form sits in the ADVANCED fold (display:none, still in the DOM), so the FX and CV MATRIX Apply buttons trigger it too. The sample sub-claim is wrong: an unlisted sample posts s:"" and drum.c:860 ignores an empty string, so the pad keeps its sample.

- Fix: Prepend <option value="0">none</option> (selected when p.src is 0) to the cvs list in rpDrumForm. For the sample select, add p.s as a selected option when it is not in names, so the dropdown shows the real state. _[web]_

**#179 CONFIRMED · wrong-audio · rare · `components/rest-api/html/index.html:1769`** — FX delay time slider max 1000 ms; firmware allows 2000, Apply shortens it

- Why: fxdelay_set_time_ms clamps only at FXD_MAX_FR-1, about 2000 ms (fxdelay.c:70-77, fxdelay.h:27), and the panel Time row steps +10 ms with no lower cap (fxrack.c:70). fxrack_save emits dlyt as that value (374), the range input built at index.html:1796 with max 1000 clamps it, and rpApply posts dlyt:1000, which fxrack_load applies (441). This only happens with Delay in FX1/FX2 (otherwise no slider is rendered) and only sticks for free-running time: with sync on and a tempo, fxrack.c:202 recomputes the time every block, so the finder's 40 bpm example self-heals.

- Fix: Change the dlyt entry in FXP to max 2000, matching FXD_MAX_SEC. _[web]_

**#45 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/html/index.html:2635`** — rpApply posts the whole load-time snapshot, reverting device-side edits since

- Why: rpData is set only in rpLoad (:2608) and after an Apply (:2656); nothing refreshes it from the 500 ms poll. rpApply clones it, overlays DOM fields that were themselves rendered at load, and posts every key, and each preset_load applies every key present (e.g. synth.c:353-357 cut/res, then cvmtx_load sets rearm=0xFFFF so the knob must move again to retake). A K6 turn or a panel Setup edit made after the page loaded is therefore rolled back by any matrix/FX/Advanced Apply. Same root as #4's Apply consequence, but #4's fix does not cover it.

- Fix: Keep a pristine copy taken at rpLoad (fxHarvest writes into rpData, so rpData cannot be the baseline) and post only keys that differ from it; preset_load already treats an absent key as keep, and cvmtx_load documents a node with no matrix as a partial update (cvmtx.c:287-289). Send mxs/mxa/mxm together when any of them changed. _[web]_

**#44 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/html/index.html:3726`** — A machine switch made on the panel leaves the old machine's cards and rpData in place

- Why: pollStatus only assigns curMach; capsApply (:3544) toggles visibility and nothing calls rpLoad on a change (callers are :1689, :2584, :2656, :2686, :3276 only). Synth, Keys and Tape share the MATRIX/FX/SETUP caps, so the stale cards stay visible, and Apply posts the old machine's full JSON to the new machine's preset_load (menu.c:618-627) with autosave. cvmtx_load bounds and clamps the arrays (cvmtx.c:290-313), so it is wrong routing and wrong shared keys, not memory corruption. A stale Setup click steps a different row once, then suLoad corrects the card.

- Fix: In pollStatus, compare d.machine with the previous curMach and call rpLoad() when it changed (it already re-runs suLoad/glLoad); remMach's 2.5 s timer then becomes a fallback. Optionally have rpApply refuse when rpData was loaded for a different machine. _[web]_

**#163 CONFIRMED · wrong-behaviour · normal-use · `components/machine_slicer/slicer.c:683`** — Web Apply reloads the sample on Slicer, Granular, Drums, Synth and Keys every time

- Why: rpApply (index.html:2633-2651) posts the whole readback including the sample name, and slicer_preset_load calls slicer_load unconditionally; slicer_load (slicer.c:633) sets loading and playing=false, and the reader (slicer.c:225-258) redoes scan_file, the .ot parse and build_heads and resets cur/sel. The same unguarded reload is in granular.c:258, drum.c:860-863, synth.c:332 and instsampler.c:749-756 (clear all zones, reverb forced off, every zone reloaded). Deck, DoubleDecker, Sampler3 and Tracker guard with strcmp (deck.c:1097, dualdeck.c:1356, sampler3.c:1328, tracker.c:939). Apply sits on the CV matrix, FX and Advanced cards, so this is ordinary web use.

- Fix: Add the strcmp-against-current-name guard the decks and Sampler3 already use to each loader. On Slicer call slicer_reslice() instead when only slices/transient/sens changed; on Keys skip the clear+reload when the zones array names match the loaded zones and just apply root/fn/loop. _[io-rest]_

**#125 CONFIRMED · wrong-behaviour · normal-use · `components/machine_editor/editor.c:304`** — Web /edit/apply repoints ed.src/in/out without updating ed.frames or ed.peaks

- Why: editor_apply writes ed.src, in_pt and out_pt (304-308) but only editor_load sets ed.frames and starts the peak scan. The web card sends the dropdown's name with the device's mirrored in/out (index.html edApply), or none when nothing is loaded, so the panel then shows B's name over A's waveform and length, or 0.00 s with out_pt=0. A TR1 or Setup audition then calls sampplay_window(0,0), which clamps to a 64-frame loop (sampplay.c:210-211), and A's crop range is applied to B. Not recorded on 09-16.

- Fix: In editor_apply, when src differs from ed.src, call editor_probe(src), set ed.frames, clamp in/out to the file with out==0 meaning F, and clear ed.peaks (or chain scan_task when the job ends). A zero probe should set_err and return, which also closes the web trigger for the fd leak. _[io-rest]_


### S. Long-filename leftovers

**#39 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/rest-api.c:434`** — Web rename still enforces the 8-char uppercase rule after long filenames

- Why: files_rename_handler (rest-api.c:432-444) still rejects any target that is not 1-8 chars of A-Z 0-9 _, while drop_sample (rest-api.c:1350) and the page's upload/Freesound filters accept up to 31 chars of a-zA-Z0-9_-. plans/long-filenames-20260913.md step 5 says to relax /files/rename and the input maxlength, and neither was done; the 09-16 review does not record it (theme A covered only buffer sizes). The finder's scenario is slightly off: through the page the user never sees the 400, because libCommit (index.html:1248) uppercases, strips and slices to 8 before sending and the input has maxlength=8 (index.html:1171), so the name is silently shortened (e.g. a Keys note suffix is lost); the 400 only shows on a direct REST call. No memory risk: name[32]/to[32] and the 80-byte path buffers fit a 31-char id.

- Fix: In files_rename_handler size name/to as SAMPLE_ID_LEN, refuse tl >= SAMPLE_ID_LEN and allow the same charset the upload path uses (a-zA-Z0-9_-). In index.html set the rename input maxlength to 31 and make libCommit use the same filter and substring(0,31) as the batch-name code at line 1382 (the batch input at 1359 also still has maxlength 8), then run html/convert.sh. Note the 'target already exists' probe will refuse a case-only rename on FAT. _[io-rest]_

**#43 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/html/index.html:1248`** — Library rename is still 8-char: an unchanged commit truncates a long name

- Why: The input at :1171 is pre-filled with the full name (maxlength=8 does not trim a scripted value), and libCommit strips to A-Z0-9_ and slice(0,8) before comparing with name, so OK/Enter on MOOG_C4_LONGNAME posts a rename to MOOG_C4_. The server has the same leftover: rest-api.c:432-437 refuses any target over 8 chars. The finder's overwrite claim is wrong: rest-api.c:449-454 refuses an existing target, so a collision gives an alert, not a clobber. No audio is lost; the long name is, and presets holding the old id are orphaned.

- Fix: Page: return early when el.value===name, and raise maxlength/slice to 31 with the same charset the upload uses. Server: change the 'tl > 8' check at rest-api.c:434 to 'tl >= SAMPLE_ID_LEN' and widen the charset to match; from_p/to_p[80] already fit 19+31+5. _[io-rest]_

**#183 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/html/index.html:1196`** — Download link always names the file NAME.RAW although /files/raw streams WAV/AIFF verbatim

- Why: The anchor hardcodes download="<name>.RAW". files_raw_handler resolves the id with sample_resolve and streams whatever container exists, as application/octet-stream with no Content-Disposition (rest-api.c:528-549). /files emits name, dir, size, bpm, dur, mtime and ot only (288-296), so the page cannot know the container. CLAUDE.md states recordings and looper saves are written as .WAV, so every REC_/LOOP/tape take downloads as a RIFF file named .RAW; index.html was outside the 09-16 review.

- Fix: In files_raw_handler add a Content-Disposition: attachment header whose filename is the basename of the resolved `path`, and change the anchor to a bare `download` attribute so the header's name is used. Alternative: keep the audio extension seen in the /files f_readdir pass (cls == 0) in fidx_t, emit it as "ext", and build the download name from it. _[io-rest]_

**#181 CONFIRMED · wrong-behaviour · normal-use · `components/rest-api/rest-api.c:94`** — Query names are never percent-decoded; DELETE /files answers 200 when nothing was removed

- Why: IDF 4.3 httpd_query_key_value copies the value verbatim (httpd_parse.c), and get_query_param adds no decode; only the /ice handler calls urldecode_inplace afterwards (rest-api.c:2407-2411). The page sends every name through encodeURIComponent, so 'PNOF#3' arrives as 'PNOF%233' in delete, move, rename, raw, peaks, /trk/get, /trk/delete and editor_web.c qparam. files_delete_handler ignores remove() results and always sends 200 (349-354), and the page then drops the row locally (index.html:1275). Page uploads and Freesound strip such characters, but tools/curate.py ids with '#' and card-copied names that gained a sidecar are listed by /files; esc() (index.html:1260) also leaves ' unescaped inside onclick strings. Not in the 09-16 review.

- Fix: Decode inside get_query_param with the existing urldecode_inplace (rest-api.c:2380): fetch into a qlen-sized temp, decode, then refuse if it does not fit buflen, and drop the five now-double decode calls in the /ice handler. Give editor_web.c qparam the same treatment, and make files_delete_handler return 404 when no remove() succeeded, as mod_delete_handler does. In the page, pass names to handlers through data-name attributes instead of quoted onclick strings. _[io-rest]_

**#185 CONFIRMED · wrong-behaviour · normal-use · `components/machine_radio/radio_web.c:117`** — Station name/url and Freesound query are sized for the decoded value but receive the encoded one

- Why: qparam copies the still-encoded value into name[RADIO_NAME_LEN = 24]; httpd_query_key_value returns ESP_ERR_HTTPD_RESULT_TRUNC when the encoded length is 24 or more, so hn is false and radio_station_add gets "custom" (119). The m3u import sends encodeURIComponent(j.name) (index.html:2798), so 'Radio Paradise Main Mix' (23 chars, 3 spaces, 29 encoded) already fails, and longer titles become 'custom' instead of being truncated. The same pattern holds for url[RADIO_URL_LEN 176] (400 'need ?url=') and fs_web.c:162-163 q[FS_QUERY_LEN 48] ('missing q', though that needs an unusually long query). The 09-16 chunk 13 note only cleared RADIO_NAME_LEN as a long-filename leftover.

- Fix: Read each encoded value into a buffer sized for the encoded form (3x the target, or a malloc of the query length as qparam already does), run the existing url_decode / urldecode, then strlcpy into the RADIO_NAME_LEN / RADIO_URL_LEN / FS_QUERY_LEN buffer; radio_station_add already truncates with strlcpy. Best done once inside qparam / q_param so every caller gets decoded, length-checked values. _[io-rest]_

**#131 CONFIRMED · wrong-behaviour · rare · `components/machine_freesound/fs_web.c:81`** — /fs/search proxy stores the %-encoded q in fsm.last_query

- Why: The handler copies the still-encoded, filtered q into fsm.last_query without touching page/results. The panel then pages with fs_search_start(fsm.last_query, page±1) (fs_menu.c:351/359), url_escape re-encodes the '%', and fsnd_preset_save persists the string. The shipped page no longer calls /fs/search (index.html:1527 uses /fs/query), so only a direct API client with a multi-word query hits it. The httpd-stack claim is not established: the measured search peak is about 5.5 KB including a cJSON parse against an 8 KB httpd stack, and this was the production search path until 09-12.

- Fix: Delete the strlcpy at :81; last_query belongs to fs_search_task (fs_machine.c:590), which sets it together with page and results. If the proxy must record it, run the existing urldecode() on a copy first. _[io-rest]_

**#19 CONFIRMED · wrong-behaviour · rare · `components/util/sampfile.c:148`** — Files with base names over 31 chars are listed under a truncated id that cannot be resolved

- Why: sample_name_id clamps to id_len-1 and returns true; both listers use it (sample_ram.c:59, sample_list_recent.c:43) and dedup on the clamped id. sample_resolve (sampfile.c:90) stats the 31-char name, finds nothing and falls back to usr/<id>.RAW, so the load fails with 'cannot open'. CONFIG_FATFS_MAX_LFN=64, so names of 32 to 64 chars copied onto the card are read in full and hit this; web uploads and Freesound are capped at 31 and do not. Not in the 09-16 review, and plans/long-filenames-20260913.md does not describe it as intended.

- Fix: Return false from sample_name_id when the base does not fit id_len, so both listers skip a file that cannot be addressed, and log it once. The import scan could rename such files to a 31-char id if they should stay usable. _[io-rest]_

**#113 CONFIRMED · wrong-behaviour · rare · `components/machine_tracker/tracker.c:68`** — Tracker browser truncates module names over 39 chars into entries that cannot be opened

- Why: has_mod_ext tests the full d_name, then lines 68-70 cut it to 39 chars; tracker_request_load passes that to fopen and load_file_psram fails with 'open failed' (131). sdkconfig has CONFIG_FATFS_MAX_LFN=64, so the affected range is 40-64 chars, not 255; longer names arrive as 8.3 aliases and load. Only card-copied files hit it, since /trk/upload clamps names to 8 chars (rest-api.c:2052). The 09-16 'Checked OK: Tracker filenames 40 chars' note covered 24-char leftovers only, not this truncation.

- Fix: In tracker_list_modules skip names that do not fit (`if (L > TRK_NAME_LEN - 1) continue;`), the refuse-not-truncate rule drop_sample already uses. Alternative: raise TRK_NAME_LEN to FATFS_MAX_LFN+1, move s_list to PSRAM and widen path[80] in load_file_psram. _[io-rest]_


### T. Drums

**#73 CONFIRMED · wrong-audio · normal-use · `components/machine_drumsampler/drum.c:553`** — drum_process early-out leaves out[] stale; audio task re-sends the last block

- Why: drum_process never writes out before the return at 553, and audio_task (audio.c:601, 676, 740) keeps one persistent out[] that is only zeroed when no machine is active, so the last written 32-frame block repeats at ~1378 Hz until a pad sounds again. After a natural sample end the stale block is only the rem<256 tail ramp (at most 12% of the last 31 frames, silent for clean one-shots, audible for samples cut at the 2 s cap). It is full level when drum_load_layer/drum_clear_layer clear p->playing under a sounding pad (660, 698), or when an FX slot or the filter is switched off while its tail rings with no pad playing. The boot-garbage part is wrong: initAudio runs before machine_activate (ui.c:344, main.c:16), so NULL-machine blocks zero out first. Not in the 09-16 review (Drums: nothing to report).

- Fix: memset(out, 0, MACHINE_BLOCK * sizeof(int32_t)) before the return at drum.c:553, the same early-out pattern as glitch.c:86, granular.c:148 and slicer.c:460. _[audio-dsp]_

**#74 CONFIRMED · wrong-audio · normal-use · `components/machine_drumsampler/drum.c:608`** — POST-filter reverb send uses a per-sample send/dry ratio that blows up

- Why: snd*lo32/dl equals the filtered send only when snd/dl is constant, i.e. one dry-routed pad; that single-pad case is bounded (ratio <= 1), so the finder's single-pad blow-up is overstated. With two overlapping pads at different rv_send the ratio is (w1*x1+w2*x2)/(x1+x2), unbounded wherever the sum crosses zero; the same happens with one pad on the FX bus (fx_on defaults true) once a slot is live, because accL then holds FX output while sndL holds the raw pad. The int64 result clamps to +/-32767 at 620-621, so the tank gets full-scale spikes and sign flips; dl==0 also drops the send to 0 for that sample. Needs non-default Send Tap = post-filter (drum_menu.c:969) with the filter engaged and reverb on; after that every overlapping hit triggers it.

- Fix: Drop the ratio and filter the send bus itself: add a second svf_t pair to dr_state_t and run svf_step on sndL/sndR with the same dr.flt_f/dr.flt_q and mode tap when rv_post && flt_live && flt_mode, svf_park otherwise. Reset the pair alongside flt_l/flt_r (drum_start and the NaN guard). _[audio-dsp]_

**#75 CONFIRMED · wrong-behaviour · rare · `components/machine_drumsampler/drum.c:473`** — Retrigger/choke queued near the sample end is dropped

- Why: trigger_pad on a playing pad only sets retrig/fade (61-68); the new hit starts only when fade reaches 0 at 452. If pos reaches len first, line 473 sets playing=false and breaks with retrig still set, so next_layer never starts, though p->hit already flashed the UI. The window is the last 32 frames for a same-layer retrig and the last 128 for a cross-layer choke (DR_RETRIG_STEP 8, DR_CHOKE_STEP 2), doubled at +12 pitch. The decay and loop-rep ends sit in the non-retrig branch and are not affected.

- Fix: At line 473, when p->retrig is set, do not stop the voice: set p->fade = 0 and continue, so the next frame's fade check at 451-452 lands the queued layer through the existing voice_setup path. _[audio-dsp]_


### X. Sample loading

**#15 CONFIRMED · wrong-audio · normal-use · `components/util/sample_ram.c:136`** — sample_load reads to EOF and ignores sf.frames, so chunks after the audio load as samples

- Why: The loop at sample_ram.c:130-145 stops only on max_frames or a zero read; sf.frames (set from the data chunk size at sampfile_int.h:73, and from COMM nframes at :124) is never used. A native 16-bit 44.1k WAV with LIST/smpl/cue chunks after `data` therefore loads those bytes as audio and returns a frame count that is too long. Such files reach the card untouched: the web page uploads WAV verbatim (index.html DEV_EXTS) and the importer leaves native files alone (sampimport.c:358). Callers are Drums, Keys, Granular and the Synth wavetable, so it shows as a click at the end of a one-shot or at the loop point. Not in the 09-16 review.

- Fix: In sample_load clamp the limit once after the probe: `if (max_frames > sf.frames) max_frames = sf.frames;`. The streaming readers already bound by sf.frames. _[io-rest]_

**#21 CONFIRMED · wrong-audio · rare · `components/util/sampfile_int.h:114`** — AIFF convert path reads to EOF, so chunks after SSND become audio

- Why: sf_aiff_parse sets src_data_len = fsize - src_data_off (sampfile_int.h:114) and ignores both the SSND chunk size and COMM nframes; the native path at :123-124 does cap by nframes, the convert path does not. samp_import_file (sampimport.c:375) turns that into src.frames, and src_read runs to that count, so any MARK/INST/COMT/ID3 chunk after SSND is decoded as PCM and appended to the converted WAV that replaces the source. The WAV parser is not affected (it uses csz, :56-58). Not in the 09-16 list (3.9 was the 8-bit sign only).

- Fix: In sf_aiff_parse, bound src_data_len by the SSND size (csz - 8 - off) and by nframes*ch*(bits/8) when nframes is non-zero, clamped to fsize - src_data_off. This is the same self-heal shape sf_wav_parse uses at :56-58 and the native AIFF path uses at :123-124; sampfile_f.c picks it up through the shared parser. _[io-rest]_

**#16 CONFIRMED · wrong-behaviour · needs-fault · `components/util/sampfile_int.h:113`** — AIFF SSND offset field is not validated; a bad value gives a wrapped data offset and huge frame count

- Why: `pos + 8 + 8 + off` is 32-bit and wraps, and when data_off lands past EOF `(uint32_t)(fsize - sf->data_off)` at :123 wraps too, so the probe returns 0 with frames near 2^30 (or an unchecked COMM nframes). The 3.6 fix hardened the chunk walk at :127 but not this field. The consequence is smaller than claimed: sample_load just gets 0 frames, and sampplay resets src and sleeps a tick per retry (sampplay.c:67, :103), so it is silence with a bogus length, not a spin; a wrapped offset plays header bytes. It needs a corrupt or hand-made AIFF, since real files have offset 0.

- Fix: Compute the data offset in int64 like the chunk walk at sampfile_int.h:127 and reject with `sf->why = "AIFF: bad SSND offset"` when it is >= fsize. _[io-rest]_


### Z. Zero-tick waits and the broadcast server

**#150 CONFIRMED · wrong-behaviour · normal-use · `components/util/sampplay.c:102`** — sampplay reader busy-spins on a full ring (pdMS_TO_TICKS(5) == 0)

- Why: pdMS_TO_TICKS(5) is 5*100/1000 = 0, so the full-ring branch is vTaskDelay(0), and a full ring is the steady state whenever f && playing. fill_once (sampplay.c:49-55) reads any space below SP_CHUNK, so each 64-frame drain triggers a 256-byte fseek+read under sd_lock instead of 1024-frame bursts. It breaks the CLAUDE.md 'Sleep at least one tick' rule; added in 6737be2 (09-12) and not listed on 09-16. The finder's idle-cleanup/40 KB alloc chain is not shown: idle WDT checks are off and the other core's idle still runs, so the class is starvation plus dribble reads, not a hang.

- Fix: Use vTaskDelay(1) in both branches, and in fill_once return unless space >= SP_CHUNK (or the read finishes the loop span), like sampler3's 'no dribble reads' guard. _[task-memory]_

**#2 CONFIRMED · wrong-behaviour · rare · `components/audio/audio.c:522`** — Broadcast stream loops ignore s_bc_srv_run, no send timeout; disable not honoured, re-enable lost

- Why: This is 09-16 finding 1.5, but it was never triaged: it is in no fix tier and not on the Watch list, no commit touches it, and the code is unchanged. The WAV loop (audio.c:522) exits only on send() < 0, bc_stream_mp3(cl, NULL) at 519 passes no run flag, and the client socket gets only SO_RCVTIMEO (501), so a disable is ignored while a client stays connected and the 12 KB stack is kept. A re-enable in that state returns at audio.c:773 on s_bc_srv_alive and the task then exits when the client leaves. Finder's 'never returns after the client closes' is overstated (a FIN/RST fails the send); consequence is wrong behaviour plus a temporary stack hold, not a permanent leak.

- Fix: Pass &s_bc_srv_run to bc_stream_mp3() (it already takes a run pointer, as icepush uses), add s_bc_srv_run to the WAV while condition, and set SO_SNDTIMEO on cl as icepush does at audio.c:821. Move run/alive onto a worker_t (worker_spawn/worker_exit) and have enable call worker_idle() briefly before spawning so an off-then-on is not dropped. _[task-memory]_

**#152 CONFIRMED · wrong-behaviour · rare · `components/wifi/espnow_probe.c:63`** — ESP-NOW probe spins flat out for hz > 100 (period < 10 ms is 0 ticks)

- Why: hz 101..500 gives period_ms 9..2, which pdMS_TO_TICKS turns into 0, so the prio-5 task (espnow_probe.c:87) loops on esp_now_send with no blocking; set_hz clamps only to 500 and the REST handler (rest-api.c:1902) adds no clamp. hz 51..99 also all run at 100 Hz. It is measurement scaffolding and tools/bench/espnow_cost.py only uses 0/25/100, so the recorded spike result is unaffected; only a hand-posted hz above 100 hits it.

- Fix: Clamp hz to configTICK_RATE_HZ in espnow_probe_set_hz and floor the delay at one tick (ticks = pdMS_TO_TICKS(period_ms); if (ticks < 1) ticks = 1). Echo the clamped hz in the POST reply. _[task-memory]_

**#153 CONFIRMED · wrong-behaviour · rare · `components/audio/audio.c:58`** — /remote/trig with ms 5..9 never asserts the soft gate

- Why: The handler clamps ms to >= 5 (rest-api.c:1728) and pdMS_TO_TICKS(5..9) is 0, so until == now and the audio task's 'now < until' test (audio.c:629-630) is false; ms 10..19 gives 0..10 ms and can miss a block. The web page only sends ms=5 as the RELEASE of a hold (index.html:2698), where an immediate deadline is what it wants, and the pulse button defaults to 40. So only a script asking for a fresh 5..9 ms pulse is affected, and the reply's now == until shows it.

- Fix: In audio_remote_trig, floor the tick count at 2 so a pulse always spans at least one audio block. The web release still works: the gate drops 10-20 ms later with no new edge. _[io-rest]_

**#5 CONFIRMED · wrong-audio · rare · `components/audio/audio.c:374`** — s_bc_r incremented by both the audio task and the stream reader without atomicity

- Why: s_bc_r is a plain volatile uint32_t: the audio task does s_bc_r++ at audio.c:374 when the ring is full, and the reader does s_bc_r++ at 415 (MP3) and 527 (WAV) from an unpinned prio-5 task, so a reader store can undo writer drops and leave w - r above BC_FRAMES; the reader also reads the slot the writer is overwriting when full. It only happens while the ring is full (client more than 1 s behind, or MP3 next to a playing Radio, which CLAUDE.md documents as not realtime). In that state drop-oldest is already skipping audio every block, so the extra artefact is a few out-of-order frames on top of existing dropouts. Real but low value.

- Fix: Make s_bc_r reader-owned: broadcast_push() only advances s_bc_w, and the two reader loops (audio.c:411 and 524) snap r forward to w - BC_FRAMES when they find w - r > BC_FRAMES before copying. _[task-memory]_
