# Code review, 2026-09-29 — second whole-codebase pass

Reviewed at `8750c96` (no code change since the 09-16 review was fixed). Scope: all our firmware,
the TFT submodule's own commits, and the web page `rest-api/html/index.html` (first time).
Out: libxmp, helix, shine, the generated `index.html.h`, machine_sampler2, `tools/`.

How it ran: 26 Sonnet finders (9 core chunks, 11 machine chunks, 6 cross-cutting lenses), each
followed by a full-model verifier that tried to refute every finding against the code, the 09-16
file and `CLAUDE.md`. 231 raw findings → 160 confirmed, 26 plausible, 18 already known from 09-16,
27 refuted. After merging duplicates: about 130 distinct issues, listed below.

`#n` is the finding id in `plans/code-review-2026-09-29.json` (same folder), which holds the
verifier's full reasoning and a fix sketch for each. **✔ = I read the cited code myself and it is
as described.** Unmarked lines carry the verifier's verdict only. (P) = plausible, trigger not
established from reading. Paths are relative to `components/`.

Nothing here is fixed. Arlo picks the themes.

## Tier 1 — crash, hang or memory corruption

### A. Reader tasks use their SD staging buffers without a NULL check
- ✔ #79 #89 #57 #65 `machine_deck/deck.c:124`, `machine_dualdeck/dualdeck.c:240`, `machine_sampler3/sampler3.c:142`, `machine_slicer/slicer.c:219` — 16 KB `MALLOC_CAP_DMA` allocated inside the task, never tested; a failed alloc is an `fread` into address 0 on the first load. `start()` has already returned OK, so the Stub fallback never engages. Needs fragmented internal RAM (the 09-16 bench saw the largest block dip to 10 KB).
- **FIXED `ff05e05`, flashed .85 09-30.** All four readers: DMA buffers allocated in start() before worker_spawn, ESP_ERR_NO_MEM -> Stub, freed after worker_stop. Bench 09-30 on .85 (`511b9f6`): `review_bench.py --audio` 0 fail; click notes all match 09-16 runs. The low-RAM path itself not forced on the unit.

### B. Reader busy-spins when the card returns a zero-length read
- ✔ #61 `machine_sampler3/sampler3.c:327` — loop-start cache build sets `worked = true` on a zero read, skipping the tick delay.
- ✔ #78 `machine_deck/deck.c:233` — ring fill `continue`s with `got == 0`.
  Both need a card read error or a pulled card; then a prio-6 task spins and can starve httpd.
- **FIXED `ff05e05` (with A).** Sampler lsc build counts work only when got > 0; Deck's fill falls through to the tick on a zero read (log rate-limited). Owed by hand: pull the card while Deck plays — web page should stay responsive.

### C. UI task can block forever on its own queue
- ✔ #27 `menu/menu.c:746` — `menuBindMachineUI` does `xQueueSend(..., portMAX_DELAY)` on the UI task's own 64-slot queue during a machine switch. A slow stop (Tape saving, Radio, Tracker) plus encoder spinning fills it; the UI then sleeps forever holding `disp_lock`.
- **FIXED `46008f3`, flashed .85 09-30.** Non-blocking bind post; any event runs a pending bind first and the triggering event is dropped as before. proof_build passed. Bench 09-30 on .85 (`511b9f6`): `review_bench.py --audio` 0 fail; click notes all match 09-16 runs. (switch test exercises ~20 switches under REST load).

### D. FX rack
- ✔ #9 `fxrack/fxrack.c:339` — `fxrack_menu_val/adj` index the effect's param table with row numbers cached for the previous effect. A web preset apply that changes the slot's effect while the FX page is open → next encoder event reads past the table or through NULL (all four hosts).
- (P) #12 `util/fxdelay.c:30`, `fxrack/fxrack.c:420` — `fxrack_load` publishes the slot kind before the slab is initialised, and init stores `bufL` before `cap`. A few-instruction window against the audio task.
- **FIXED `00116dc`, flashed .85 09-30.** #9 param bounds-checked against the kind's table + EV_ENTERED_MENU after EV_REMOTE_PRESET; #12 kinds published after init, bufL stored last behind a barrier. Not reproduced on the unit (needs the FX page open during a web Apply) — owed by hand: Keys FX page open, Apply a preset with that slot Off from the web, turn the encoder.

### E. Tape buffer freed or edited under the save task
- ✔ #98 `machine_tape/tape.c:580` — `tape_set_len_sel` and the in-place edits gate on "transport stopped" only; changing Tape Len (or a preset `lsel`) during the multi-second autosave frees the banks `save_task` is reading.
- **FIXED `6ef2d8a` (with L), flashed .85 09-30.** Edits refuse while `save_busy` (Setup rows read `saving...`); Tape Len and `tape_load_inner` go through `tp_settle_saves()` first (queued autosave, in-flight writer, then a dirty take). Bench: Tape test passed, no crash. Owed by hand: change Tape Len during a long autosave.

### F. Looper
- ✔ #106 `machine_looper/looper.c:401` — bounce reads volatile `t->len` twice; a TR1 re-arm between the two is a divide by zero.
- ✔ #104 `machine_looper/looper.c:316` — `looper_save_track` does all its SD I/O with no `sd_lock` (the house rule; corruption itself not established).
- **FIXED `511b9f6`, flashed .85 09-30.** busy_mask makes apply_cmd ignore tracks being saved/bounced (#105 #106), bounce snapshots lengths; save under sd_lock with every write checked, partial file removed, menu FAILED (#104 #110). Checked: record + `/looper/save` -> 200, file appears. Card-full path not forced.

### G. MP3 import
- ✔ #13 `util/mp3.c:29`, `util/sampimport.c:300` — no mutual exclusion on `samp_import_file`, and the decoder's `input`/`readPtr` are file-scope globals. Freesound converting while the import scan runs → use-after-free and double free. This is 09-16 finding 3.4, which was recorded but never triaged or fixed.
- ✔ #14 `util/mp3.c:100` — sync search always scans 4096 bytes even after a short read; a match in the stale tail wraps `nRead - offset` in `memmove`. Introduced by the 09-16 fix for 3.5. Needs a sub-4 KB tagged file.
- **FIXED `e83de45`, flashed .85 09-30.** Decoder buffer per call + samp_import_file mutex (#13); sync search bounded by nRead (#14); resync on a damaged frame, decode returns a status (#17). Checked: 6 s 128 k MP3 with 1500 random bytes mid-file imports at 98% length (1,050,668 vs 1,069,100 B) — the old code stopped at the damage. Bench import test: ID3 mp3 + 24-bit WAV convert.
- **NEW 09-30 — FIXED `437f67d` (retry from the UI poll):** the import task needs a 20 KB internal stack; with Tracker active the largest internal block is 19,456 B, so the upload's kick fails silently and `POST /import` answers `{"why":"busy"}`. An upload under Tracker stays unconverted until the next scan. `review_bench.py --only import` hits it (its snapshot pass ends on Tracker). Fix direction: report spawn failure distinctly, and retry the kick later (or PSRAM-backed stack is not an option for helix — measure).

### H. Values from a preset, sidecar or config used without a range check
- ✔ #118 `machine_granular/granular.c:255` — density unclamped; a huge value from the web form stalls the audio task, and it autosaves, so it returns every boot.
- ✔ #84 `machine_deck/deck.c:362` — `deck_seek_beats` divides by `beat_tf` with no zero guard (needs an absurd sidecar bpm). #94 `machine_dualdeck/dualdeck.c:755` — same source makes `rate_sm` NaN.
- ✔ #31 `menu/menu_config.c:29` — `wifiSettingsChanged` pairs settings by position and `strcmp`s a NULL; panics on every Settings exit if CONFIG.JSN was hand-edited with a number ahead of `ssid`.
- ✔ #117 `machine_tracker/tracker.c:300` — `xmp_create_context()` NULL unchecked; crash on leaving Tracker when PSRAM was exhausted.
- (P) #147 `machine_slicer/slicer_ot.c:137` — `.OT` export loop bound re-reads volatile `n_slices`; can overrun an 832-byte stack buffer.

## Tier 2 — silent data loss or wrong saved state
- **FIXED `5ca0d5f`, flashed .85 09-30** (#118 #84 #94 #31 #117 #147). Checked: Granular `density: 1e9` reads back 120, `aus` ~940 µs, no reboot. Others are code-only (need a hand-edited sidecar/config or exhausted PSRAM).

### J. JSON files are rewritten unsafely
- ✔ #20 `util/fileio.c:319` — `writeJSONFile` truncates in place, checks nothing, returns void. A power cut in the ~64 ms write empties CONFIG.JSN (WiFi, encoder settings) or AUTOSAVE.JSN; a truncated AUTOSAVE.JSN then blocks every later autosave.
- ✔ #24 `menu/menu.c:914` — a failed AUTOSAVE.JSN read at bind loads defaults, and the next autosave writes them over the machine's entry.
- ✔ #18 `util/fileio.c:205` — `getFileSize` returns stack garbage when the file is missing; on a fresh card autosave may never create the file. (P on the garbage value)
- #29 `menu/menu_config.c:62` — CONFIG.JSN read-modify-write not locked across UI and httpd. (P)
- #86 `machine_deck/deck_analysis.c:54` — sidecar read-modify-write races a Feel edit; `feel` is lost.
- **FIXED `f87f055`.** writeJSONFile: X.JTM -> check -> park X.JBK -> rename in -> drop backup, returns status; reads fall back to X.JBK; autosave/preset store act on it (#20); bind retries a failed AUTOSAVE read and then holds autosave off (#24); getFileSize 0 for missing (#18); a dedicated recursive `config_lock` around every CONFIG.JSN read-modify-write — not sd_lock (would stall the readers) — and the web handler applies the TFT clock after unlocking, because the UI task takes config_lock under disp_lock (#29); Deck sidecar RMWs under sd_lock (#86). Flashed .85; full `review_bench.py --audio` on `476df9f`: 0 fail.
- **NEW 09-30 — FIXED `437f67d` (1 KB DMA bounce into PSRAM):** with Deck or DoubleDecker active, `readJSONFileAsCJSON(AUTOSAVE.JSN)` fails `Could not allocate memory` (the internal DMA buffer for the ~5 KB file), so autosave skips ('read failed on existing file') — settings made on the decks may not save. Seen on serial during the decks test; the 09-16 notes recorded the same message as unexplained. Fix direction: read in chunks into a PSRAM buffer through a small DMA bounce, or keep a parsed copy.

### K. Settings that are not saved, or wiped by a partial update
- #167 `machine_slicer/slicer.c:665` — Slicer saves no FX box, reverb mode/mix, or OT/grid choice.
- #168 `machine_drumsampler/drum.c:745` — Drums saves no knob-7 attack / start / loop values.
- #169 `machine_granular/granular.c:244` — Granular saves no position, pitch or level.
- #164 `machine_instsampler/instsampler.c:756`, `drum.c:672` — a zone or pad that fails to load (arena short) is dropped from the next autosave.
- ✔ #162 `fxrack/fxrack.c:429` — a preset body with no `fxsl` sets both FX slots Off and frees their slabs (bench scripts post partial bodies; the page does not).
- ✔ #166 `machine_tracker/tracker.c:919` — Tracker resets file, flags and CV matrix before reading a partial node; Freesound's lists likewise.
- **FIXED `49ebd64` + `476df9f`.** fxrack partial-update rule (#162); Tracker/Freesound defaults only for no saved state (#166); Granular pos/pitch/level, Drums atk/sto/lpm, Slicer fx/rv/rvmx/otoff saved (#167-#169); failed Keys zones / Drums pads stay in the patch (#164). The follow-up `476df9f` frees Keys' unloaded-zone list (cJSON, PSRAM) on stop — left behind it pinned the pool and Looper's 4th track failed after a switch away mid-load (found on the bench). Flashed .85; full `review_bench.py --audio` on `476df9f`: 0 fail.

### L. Tape
- ✔ #97 `machine_tape/tape.c:722` — **Cut deletes the whole crop when the clipboard allocation was trimmed.** A 40 s tape leaves room for about 4.4 s of clipboard; any longer Cut loses the rest, no message. Normal use.
- #100 `tape.c:749` — Norm / Rev / Fade / Cut / Paste never mark the take dirty, so edits are dropped on leave or reboot. A side effect of the 09-16 fix for 11.5. Normal use.
- ✔ #102 `tape.c:400` — momentary record: gate held past tape-full starts a fresh take over the one just queued for saving.
- ✔ #96 `tape.c:409` — TR1 "plays" a blank tape; TR2 then starts an overdub that records nothing.
- #103 `tape.c:902` — crop load-back runs after a failed or short save.
- #95 `tape.c:187` — `rec_stop_target` never cleared by stop / erase / a fresh punch-in. #99 `tape.c:901` — adopt kick forces `playing = false` while recording. #101 `tape.c:760` — Crop Beats with clock OFF collapses the crop to 64 frames.
- **ALL FIXED `6ef2d8a`, flashed .85 09-30.** Edits call `tp_edited()` (dirty + `dirty_gen++` + clear `restore_id`); Cut refuses unless the whole crop was copied (row reads `no room`); adoption needs `save_ok`, raises `loading` + `machine_block_wait()` before checking `recording`, resumes playing on a failed load-back; `full_latch` for momentary tape-full; fresh take when `len == 0`; `rec_stop_target` cleared in stop/erase/fresh and momentary no longer re-requests while one is pending; Crop Beats returns on no clock (row `no clock`). Bench (`review_bench.py --audio`): 0 fail, Tape click note = the same near-zero step as on 09-16. Open: an edit made since the last save is still lost on a REBOOT (only leave/load saves it) — saving per edit would mint a CUT_ per Norm/Rev/Fade; left as is, Arlo to decide. Owed by hand: edit a take, switch machine, come back — the edit should be there as a new CUT_.

### M. Recording service and OTA
- ✔ #37 `rest-api/rest-api.c:2469` — `POST /ota` does not refuse while recording (`/reboot` does).
- ✔ #4 `audio/recording.c:111` — a take index above 9999 sends every take to one `<PFX>OVFL.WAV`, truncated each time. One date-named file such as `REC_20260913.WAV` triggers it.
- #1 `audio/recording.c:137` — a stop during the writer's file setup strands it; bounce is refused until reboot.
- #7 `audio/recording.c:203` — a failed take leaves its header-only WAV in `usr/REC`.
- **FIXED `94539b3`.** /ota refuses while recording (#37); no 4-digit take cap + sample_next_index counts only <prefix><digits>.<ext> (#4); writer parks on a trigger latch (#1); a take with nothing captured removes its header-only WAV (#7). Flashed .85; full `review_bench.py --audio` on `476df9f`: 0 fail. Not forced: a take past 9999, a stop inside the writer's setup.

### N. Import, upload and file handling
- ✔ #182 `rest-api/rest-api.c:2051` — `/trk/upload` still cuts module names to 8 chars and overwrites a same-prefix module. Normal use.
- ✔ #17 `util/mp3.c:207` — MP3 decode stops at the first bad frame, ignores `f_write`, reports success; the truncated result replaces the source.
- #137 `util/sampimport.c:275` — re-uploading under an existing name is refused by `imp_publish` and left unconverted. #36 `rest-api.c:1478` — an upload finishing during an import scan is never converted.
- ✔ #124 #123 `machine_freesound/fs_web.c:148`, `fs_menu.c:437` — `/fs/fetch` has no name-collision check: it installs a shadowed `X.WAV` over `X.RAW`'s sidecar, and Drop Last can then delete the user's own sample.
- ✔ #184 `rest-api.c:348` — delete / rename / move sweep an id across all seven folders; twins in two folders go together.
- #47 `html/index.html:1346` — batch upload names can collide after sanitise/truncate; the later file overwrites.
- ✔ #38 `rest-api.c:2172` — `PUT /drop_ot` ignores `f_write` (incomplete 09-16 fix of 6.8). (P) #138 `rest-api.c:1455` — `drop_sample` ignores `f_close`.
- ✔ #110 #105 `machine_looper/looper.c:333`, `:326` — Looper save ignores write errors and reports SAVED; a re-arm during the save writes an empty file.
- ✔ #22 `util/fileio.c:82` — boot sweep deletes `IMPBAK.TMP`, the in-place import backup (few-ms window).
- #52 `html/index.html:1506` + `rest-api.c:1097` — Save posts `ssid:""` when the row is blank and the firmware stores it (unit falls back to AP).
- #133 `machine_freesound/fs_machine.c:153` — sidecar write failure ignored.

- **FIXED `842d236`.** See the commit for the list (#137 #36 #22 #182 #38 #138 #184 #52 #123 #124 #133 #47). Checked on the unit: a 36-char module name -> 400; a re-upload under the same name converts and replaces the WAV (it used to sit as a shadowing .RAW); an id moved to DRUMS then re-uploaded -> 409 'Name already used in DRUMS'. Flashed .85; full `review_bench.py --audio` on `476df9f`: 0 fail. Page checked with `node --check`; not clicked through. NOT live-tested: `ssid:""` (a failure would drop the unit off the LAN).
## Tier 3 — wrong audio or behaviour

### O. Reverse is broken in Sampler and Slicer (normal use)
- ✔ #56 `machine_sampler3/sampler3.c:78` and #63 #64 `machine_slicer/slicer.c:131` — the reader reverses each chunk in place with `dst == stage` and no swap, so every ~93 ms chunk comes out as a mirror-fold, not reversed audio. Heads, loop-start cache, slice heads and tails.
- **FIXED `2943663`, flashed .85 09-30.** Two-ended swap in `stage`, copy out only when `dst != stage`. Host test on a ramp (1..4096 frames, in place and offset). **On the unit:** a 4 s 300→3000 Hz log chirp (`REVTST`) through Sampler V1 captured on the Scarlett: forward rises cleanly, reverse falls 3000→300 with ONE rising step in ~610 frames (the loop wrap) — the old fold would show one per 93 ms chunk. Slicer runs the identical code; not captured. Ear: optional, Sampler/Slicer reverse on a known take.

### P. Web page: cards that throw on a missing element (normal use)
- ✔ #41 `html/index.html:2750` — `rdState()` dereferences `#rdnote`, which does not exist: the Radio card never renders state or stations.
- ✔ #42 `:3241` — `edState()` same with `#ednote`: the Editor card never builds its op buttons.
- ✔ #40 `:2575` — `webPoll()` same with `#ptag`: the Poll row never shows PAUSED.
- **FIXED `1c81628`, flashed .85 09-30.** `#rdnote`/`#ednote` lookups dropped (the cards only show while their machine is active); `#ptag` restored as a fixed top-right tag. Served page checked: no direct `getElementById` lookup of an id missing from the markup. Not yet clicked through in a browser (Chrome extension was offline).

### Q. Web page: Apply sends something other than what the device holds
- ✔ #176 `:1764` — FX3 dropdown has no Shimmer; any Apply on a Shimmer patch posts `rv:0` and turns the reverb off.
- #178 `:2199` — Drums pad level slider max 255, firmware goes to 1023; Apply clamps driven pads. #177 `:2188` — Drums trig select has no "none". #179 `:1769` — delay slider max 1000 ms vs 2000.
- #45 `:2635` — `rpApply` posts the whole load-time snapshot, reverting device-side edits since.
- #44 `:3726` — a machine switch on the panel leaves the old machine's cards live on the page.
- #163 `machine_slicer/slicer.c:683` — web Apply reloads the sample every time (Slicer, Granular, Drums, Synth, Keys).
- #125 `machine_editor/editor.c:304` — web `/edit/apply` repoints the source without updating frames or peaks.
- **FIXED `716553e`.** Page items by a Sonnet helper (diff reviewed; `node --check` clean): Shimmer listed, Drums level 1023 / trig none, delay 2000 ms, Apply posts only keys changed since load (s3v always), panel switch reloads the cards. Loader guards in Slicer/Granular/Drums/Synth/Keys (#163); Editor apply probes a new source (#125). Flashed .85; full `review_bench.py --audio` on `6678b95`: 0 fail. Page not clicked through (Chrome extension offline) — owed by hand: edit a knob on the unit, then Apply an unrelated FX change from the page; the knob value should survive.

### R. Web page: other
- #46 `:2968` — musical typing eats Cmd/Ctrl shortcuts and sticks notes on blur or octave change.
- #49 #54 `:1197`, `:1536` — names with `'` or `\` break inline `onclick` handlers.
- #48 `:1348` — batch list can be changed mid-upload. #51 `:1592` — Freesound poll never stops after a switch. #55 `:1223` — waveform cache never invalidated. (P) #50 `:3752` — status poll has no in-flight guard. (P) #53 `:1301` — AudioContext leak on decode failure.
- **FIXED `cd4574a` + `220a59f`** (page by a Sonnet helper, diff reviewed, `node --check`). Full `--audio` bench on `d69c3ed`: 0 fail. Not clicked through.

### S. Long-filename leftovers
- #39 #43 `rest-api.c:434`, `html/index.html:1248` — web rename still enforces 8 uppercase chars; an unchanged commit truncates a long name. Normal use.
- #183 `html/index.html:1196` — download link always names the file `.RAW` though WAV/AIFF are served as is.
- #181 `rest-api.c:94` — query names never percent-decoded; `DELETE /files` answers 200 when nothing was removed. #185 `machine_radio/radio_web.c:117` — station name/url and Freesound query sized for the decoded value but hold the encoded one. #131 `fs_web.c:81`.
- #19 `util/sampfile.c:148` — file stems over 31 chars are listed under an id that cannot resolve. #113 `machine_tracker/tracker.c:68` — same for module names over 39.
- **FIXED `a0d82c1`.** Checked on the unit: rename to `Review-Rename_LongName_0123` -> 200 and listed; download header `filename="...0123.WAV"`; `%2D` in a name resolves; DELETE of a missing name -> 404. Flashed .85; full `review_bench.py --audio` on `6678b95`: 0 fail.

### T. Drums
- ✔ #73 `machine_drumsampler/drum.c:553` — `drum_process` returns early without writing `out[]`; the audio task re-sends the last block. Silent if the sample ended in silence, a 689 Hz buzz if it did not.
- #74 `drum.c:608` — post-filter reverb send rebuilt from a per-sample send/dry ratio that is unbounded.
- #75 `drum.c:473` — a retrigger/choke queued near the sample end is dropped.
- **FIXED `3a045c3`** (#73 #74 #75). Flashed .85; full `review_bench.py --audio` on `6678b95`: 0 fail. Not captured: the buzz case needs a pad cut mid-sound. Ear: Drums with reverb Send Tap = post and two pads at different sends — the tail should sound like before, minus any crackle.

### U. Sampler
- #58 `machine_sampler3/sampler3.c:344` — a seek sets `wpos = tgt`, so never-streamed ring frames below the seek point pass as valid.
- #59 `sampler3.c:519` — stop leaves the record arm / a running take behind on a machine switch. #60 `:36` — `s_discard_next` can eat the next good take.
- **FIXED `cce4dab`** (#58 #59 #60). Full `--audio` bench on `d69c3ed`: 0 fail.

### V. Deck and DoubleDecker
- #82 #92 `machine_deck/deck.c:463`, `machine_dualdeck/dualdeck.c:206` — loop engage/release truncates `wpos` while the reader has a chunk in flight; the stale chunk lands at the new position. Normal use. #80 `deck.c:558` — same family in `deck_loop_remap`.
- #83 `deck.c:341` — resume from pause inside a loop lands at the window start. #81 `deck.c:458` — freeze release ignores a pending scheduled window.
- #91 `dualdeck.c:373` — loading a track does not clear the old loop window. #90 `dualdeck.c:121` — a failed open leaves `loading` set forever. #87 `dualdeck.c:917` — armed start/stop never fire on a slow PPQ-1 clock. (P) #93 `dualdeck.c:336`.
- #85 `deck.c:955` — 32-bit playback counters wrap after about 27 h.
- **FIXED `8b71683`** (#82 #92 #83 #81 #91 #90 #87 #93; #80 in-flight half). NOT done: #80 second-move half, #85 27 h wrap. Full `--audio` bench on `d69c3ed`: 0 fail.

### W. FX and DSP
- #11 `fxrack/fxrack.c:420` — a preset may put the same effect in both slots; the one instance then runs twice per block.
- #170 `util/fxdelay.c:28` — delay/flanger init loses defaults (Tape sync/div wiped on first use). #171 `fxrack.c:411` — load restarts the reverb on an unchanged mode.
- #8 `util/flanger.c:112` — float wrap can round the read position to exactly `cap`, one past the line.
- #10 `fxrack.c:131` — the CV-modulated value is visible to save and menu mid-stage and can be stored as the base.
- **FIXED `23ce22b`** (#11 #171 #170 #8). NOT done: #10 (needs per-effect CV offsets). Full `--audio` bench on `d69c3ed`: 0 fail.

### X. Sample loading
- #15 `util/sample_ram.c:136` — `sample_load` reads to EOF rather than `sf.frames`, so WAV/AIFF chunks after the audio (LIST, ID3) load as samples: a click at the end of Drums/Keys/Granular/Slicer loads. Normal use with DAW-exported files.
- #21 `util/sampfile_int.h:114` — same for AIFF conversion. #16 `:113` — AIFF SSND offset not validated.
- **FIXED `1069aa3`** (#15 #21 #16). Flashed .85; full `review_bench.py --audio` on `6678b95`: 0 fail. Not forced with a DAW file carrying LIST/smpl chunks — ear: a DAW-exported one-shot in Drums should end without a click.

### Y. Keys, Synth, Looper, Radio: preset values not clamped, and smaller state bugs
- #67 `machine_instsampler/instsampler.c:366` — zero-cross snap always uses zone 0. Normal use on multi-zone patches.
- #68 `instsampler.c:523` — LoopMov can push the loop past the sample end; the voice goes silent. #69 `:774` — peaks show the last zone after a preset load.
- #71 #72 `instsampler.c:785`, `:709`; #76 #77 `drum.c:813`, `:835`; #119 `machine_synth/synth.c:340`; #109 `looper.c:457`; #112 `machine_radio/radio.c:477` — preset values stored unclamped.
- #111 `radio.c:297` — Radio never checks the HTTP status; a 30x/404 reconnects forever. #114 `radio.c:387` — `radio_play_url` zeroes ring counters under a live drain.
- #116 `tracker.c:240` — order map stops at the first skip marker (S3M/IT). (P) #115 `tracker.c:253`.
- #107 `looper.c:423` — TR1 on empty track 1 during the bounce swap. #108 `machine_glitch/glitch.c:55` — ring never zeroed.
- Editor: #121 #122 `machine_editor/editor.c:210`, `:483` FILE leaks on a zero-frame source; #129 `:188` partial read dropped; #130 `:582` start wipes state under a surviving job; #33 `editor_menu.c:181` Live strip not repainted after the peak scan; (P) #128.
- Freesound: (P) #127 `fs_machine.c:74`, (P) #132 `fs_web.c:107`.
- **FIXED `3bed9ce`** (all listed incl. Editor/Freesound plausibles). Radio verified playing on the new connect path. Full `--audio` bench on `d69c3ed`: 0 fail.

### Z. Zero-tick waits and the broadcast server
- #150 `util/sampplay.c:102` — audition reader busy-spins on a full ring (`pdMS_TO_TICKS(5)` is 0). Normal use in Editor/Freesound audition.
- #2 `audio/audio.c:522`, `:530` — the :8000 stream loop ignores the disable flag, has no send timeout, and busy-spins between blocks. 09-16 finding 1.5, recorded but never triaged.
- #152 `wifi/espnow_probe.c:63` — probe spins for hz > 100. #153 `audio/audio.c:58` — `/remote/trig` with ms 5..9 asserts nothing.
- #5 `audio/audio.c:374` — `s_bc_r` incremented from two tasks. (P) #6 `audio.c:581`.
- **FIXED `6678b95`** (#150 #2 #5 #152 #153; #6 not changed — plausible only, fold into a later trig edit). `/remote/trig?ms=5` now spans 2 ticks (now 45934 -> until 45936). Flashed .85; full `review_bench.py --audio` on `6678b95`: 0 fail. Broadcast disable-while-listening not exercised.

### Menu and display
- #26 `menu/menu.c:365` — Settings re-entry leaks the CONFIG tree (09-16 4.3, still present). #32 `menu.c:562` — text entry finds ssid/passwd/apikey by position. #30 `menu/menutft.c:160` — unterminated `strncpy` before `strlen`.
- #62 #88 `sampler3.c:133`, `dualdeck.c:229` — a -32768 sample wraps the thumbnail column to 0.

- **FIXED `945e367`** (#26 #32 #30 #62 #88); `d69c3ed` drops the second repaint after a web preset on the main/live page (Arlo: double flash). Full `--audio` bench on `d69c3ed`: 0 fail.
## Watch
- `sd_lock` gaps where corruption was not established (FatFS is built reentrant): #3 `recording.c:106` mkdir, #23 `sampfile.c:91` `sample_resolve` stat walk, #35 `sample_list_recent.c:22` lock held across all folders, #157 `sampimport.c:246` PSRAM buffers written through the bounce path.
- #66 `slicer_web.c:29` — `.OT` export during a load returns a stale table.

## Owed by the 09-16 review
Three of its findings were recorded but never reached a fix tier: 1.5 (broadcast, #2), 3.4 (import mutex, #13), 4.3 (Settings leak, #26). Two of its fixes introduced something: 3.5 → #14, 11.5 → #100. One was incomplete: 6.8 → #38.
