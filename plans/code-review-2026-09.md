# Code review, 2026-09 — whole firmware, in chunks

First review of our own code. Collect findings for all 13 chunks, triage afterwards, fix later
(one machine at a time, bench in the loop). Each chunk: `/code-review medium <paths>`, no `--fix`.

Excluded: libxmp, helix, shine (vendored) · `rest-api/include/index.html.h` (generated) ·
`rest-api/html/index.html` · machine_sampler2 (not built) · upstream TFT code + fonts ·
`hardware/`, `tools/`, bench logs.

Finding format: `file:line` — summary — failure scenario — CONFIRMED/PLAUSIBLE.

## Checklist
- [x] 1 Audio engine + machine core — `components/audio`, `components/machine`, `main`
- [x] 2 Shared DSP + FX rack — util: reverb, flanger, fxdelay, fxfilter, svf, tremolo, overdrive, lfo, pitch_detect, bpm_analysis, fxchain.h; `components/fxrack`
- [x] 3 Sample + file I/O — util: sampfile, sampfile_f, sample_ram, sampimport, mp3, fileio, sd_lock, list, preset_store, sampplay, sample_list_recent, string_tools, c_timeutils, timer_utils, disp_lock
- [x] 4 Menu framework — menu.c, menutft.c, menusys.c, menu_config.c, setup_menu.c + headers; `components/ui`
- [x] 5 Shared widgets — cvmtx, wave_edit, text_entry, sample_browser
- [x] 6 Network + REST — `rest-api/rest-api.c`, `components/wifi`, `components/freesound`
- [x] 7 TFT library, our commits — submodule `b1489d3..HEAD` (base confirmed: b1489d3 is the last upstream commit)
- [x] 8 Samplers A — machine_sampler3, machine_slicer
- [x] 9 Samplers B — machine_instsampler (Keys), machine_drumsampler
- [x] 10 Decks — machine_deck, machine_dualdeck
- [x] 11 Tape + Looper — machine_tape, machine_looper
- [x] 12 Synth-type — machine_tracker, machine_synth, machine_granular, machine_glitch
- [x] 13 Streaming + editing — machine_freesound, machine_editor, machine_radio

## Findings

### 1 Audio engine + machine core
_Reviewed 09-16 at `55d0393` (last commit touching these paths). Unverified — reviewer's severity, no verify pass._

1. **High** `machine/machine_core.c:56` — `machine_activate()` stops the old machine before the new `start()`; on failure nothing is active, no Stub fallback. `menuSwitchMachine` (`menu/menu.c:284`) returns without rebinding → old pages act on freed state → silence + likely crash on next encoder event.
2. **Med** `main/main.c:16` — boot ignores `machine_activate()` return; saved machine failing to start leaves no machine, no Stub fallback.
3. **Med** `audio/audio.c:165` — `audio_bounce_start()` checks only `recording_is_active()`, not an armed take. With a Sampler take armed, bounce triggers the Sampler's take: records output bus under `REC_`, auto-loads into the voice.
4. **Med** `audio/recording.c:254` — writer `xTaskCreate` unchecked while `rec_prepared` already true. On failure (low internal RAM, e.g. bouncing Radio) `rec_prepared` sticks; queue fills undrained; no recording/bounce until reboot.
5. **Med** `audio/audio.c:761` — broadcast off→on quickly is lost (`s_bc_srv_alive` still true, task exits anyway). WAV/MP3 loops (513–528) never check `s_bc_srv_run`, `send()` has no timeout → stalled client keeps task + 12 KB internal stack after disable.
6. **Low** `audio/audio.c:172` — bounce ending on its own (card-full) leaves `s_bounce` true: prefix stays BNC, later Sampler takes use output tap, new bounce refused until stop.
7. **Low** `audio/recording.c:276` — `recording_cancel_prepared()` check-then-set race with audio-task `recording_trigger()`: file deleted but `rec_active` stuck true; takes refused until `recording_stop()`.
8. **Low** `audio/audio.c:117` — `audio_midi_note_on()` touches alive-stamp after publishing the note; first note after >5 s silence can be dropped by `audio_midi_gate()` → all-off.
9. **Low** `audio/audio.c:599` — `ctrlData` uninitialised; first block's `io.cv` is stack garbage → bogus clock floor / knob takeover seed / phantom clock edge.
10. **Low** `machine/clock.c:392` — with `clk_auto`, INT fallback after 2 s; an external clock slower than 1 pulse per 2 s (PPQ 1 < 30 BPM) never locks.

### 2 Shared DSP + FX rack
_Reviewed 09-16. Unverified — reviewer's severity, no verify pass._

1. **High** `util/fxfilter.c:13` — SVF unstable at top of range (coef×damping reaches 2; ceiling 1.3 allows it). Filter: reso 0–20% + cutoff >~90% diverges (sim blew up in 61–75 samples at 100%). Band: width 100% + base >~75%, CV offset can push there. State stays inf/NaN until machine restart; `fx_pack_softclip` float→int of NaN is UB. Fix: clamp coef against damping + NaN reset like flanger/delay.
2. **Med** `fxrack/fxrack.c:305` — `slot_gc` frees flanger slab; re-enabling calls `flanger_init` which zeroes rate/depth/fb/mix/sync/div → defaults. Undoes the fix at 443–448 (preset with FX1 Off but flanger dialled in).
3. **Med** `fxrack/fxrack.c:370` — same for delay: re-enable resets settings incl. sync/div; while freed `cap`=0 so `fxdelay_time_ms()`=0 and autosave writes `"dlyt":0`; `fxrack_load` (430) still gates dly* keys on allocation.
4. **Low–Med** `util/pitch_detect.c:245` — unsigned `j -= 3` wraps when `a_first`=0 and `frames % 3 == 2` → reads backwards out of buffer (crash or huge `a_last`). E.g. Keys Tune-on-Load on a silent take. Fix: `j >= a_first + 3` or signed index.
5. **Low** `util/lfo.c:56` — synced "rnd" shape: cycle reset sets phase 0 before advance, so wrap is missed when pulse boundary beats phase 1.0 → S&H holds a value for 2+ divisions.

Checked, not reported: reverb `s_tap` static shared across instances (safe, one machine at a time); flanger/delay/reverb positions in bounds; bpm_analysis indexing in bounds; fxrack CV offsets restored correctly.

### 3 Sample + file I/O
_Reviewed 09-16 at HEAD `ae6681a`. Unverified — reviewer's severity, no verify pass._

1. **High** `util/sample_list_recent.c:18` — alloc is `MAX * 24` bytes but entries are `char[SAMPLE_ID_LEN]` = 32 since `4722ac9` (LFN). >384 ids in a dated browse → `strcpy` past the block → PSRAM heap corruption.
2. **Med** `util/sample_list_recent.c:76-84` — newest-first sort still copies 24 bytes per entry; 24–31-char names lose terminator → mangled tail, id doesn't resolve. (Same LFN leftover as #1.)
3. **Med** `util/sampimport.c:307, :346` — `remove(src); rename(tmp, dst)` unchecked; if `<name>.WAV` exists (same Freesound name twice, `X.AIF` next to `X.WAV`) rename fails, source already gone, returns 0, audio left in `IMPNEW.TMP` which boot sweep deletes.
4. **Med** `util/sampimport.c:278` — no guard against concurrent conversions: `samp_import_file` ignores `samp_import_busy`, Freesound (`fs_machine.c:334`) calls it directly. Shared temp files + global `input` buffer in `mp3.c` → mixed audio or double free.
5. **Med** `util/mp3.c:130-131` — sync-word `offset` discarded; decode starts at `input[0]`. MP3s with ID3v2 tag / leading padding (which `magic_is_mp3` accepts) fail "mp3 decode failed".
6. **Med** `util/sampfile_int.h:69` (also `:114`) — chunk walk `long pos += 8 + csz + pad` wraps; corrupt size `0xFFFFFFF8` → pos never advances → infinite loop under `sd_lock` → whole SD bus hung incl. audio streaming.
7. **Med** `util/sampplay.c:58-60` vs `:179-184` — `sampplay_close` nulls `f`, waits 30 ms, frees `stage`; reader blocked in `sd_lock_take()` >30 ms wakes to `fseek(NULL)` / read into freed stage → crash.
8. **Low** `util/sampplay.c:133-137` — `sampplay_destroy` waits max 1 s for reader then frees `p` anyway; reader behind a long lock holder (`sample_list_recent_dir` holds it across 7 folders) → use-after-free.
9. **Low** `util/sampimport.c:62` — 8-bit path always unsigned (−128); AIFF 8-bit is signed → sign-flipped distortion.
10. **Low** `util/sampimport.c:326` + `sampfile_int.h:39` — `WAVE_FORMAT_EXTENSIBLE` (0xFFFE) not convertible; most DAW 24-bit/float WAVs rejected.
11. **Low** `util/fileio.c:243/263` — JSON buffer allocated at file size with no terminator, passed to `cJSON_Parse` (strlen) → reads past block on every config/preset load.

Not reported: legacy `list.c`, `getFilesInDir`, `string_tools.c`, `timer_utils.c`, `fixed.h` issues — only callers are in Sampler2 (not built).

### 4 Menu framework
_Reviewed 09-16 at HEAD `ae6681a`. Unverified — reviewer's severity, no verify pass._

1. **High** `menu/menu.c:447` — long-press out of Settings doesn't check the config loaded: `cJSON_Print(NULL)` (466) and `->valuestring` on NULL (468) → crash. If it loaded once before, static `settings` points at config freed at 485 → next failed read runs Replace/Delete/`apikey` on freed memory → heap corruption.
2. **Med** `menu/menu.c:466` — leaving Settings writes back the snapshot taken on entry, rolling back concurrent writes (web POST `/settings` hostname/txpwr/tz, `configSetIntSetting` tftclk/clock, `configSetStringSetting("machine")`). Also leaks the `cJSON_Print` buffer each exit.
3. **Med** `menu/menu.c:590` — after a web Setup edit the device re-enters whatever page is on screen: typing an SSID/API key loses the text; Settings re-reads CONFIG.JSN (leak) and resets unsaved choices; Setup page drops `[ ]` edit mode.
4. **Med** `menu/menu.c:283` — machine-pick short press arms the 2 s autosave timer; `autosave_now` doesn't stop it. If `machine_activate` >2 s (SD loads), `EV_AUTOSAVE` precedes `EV_MACHINE_BIND` → new machine's AUTOSAVE.JSN entry overwritten with defaults.
5. **Low** `menu/menu.c:858` — every bind enters the landing page twice (`menusys_set_active_item` + `EV_ENTERED_MENU`) → two full repaints per boot/switch.
6. **Low** `menu/menu.c:789` — tuner only disabled on long-press; web machine switch with Tuner page open leaves pitch detection running until reboot.
7. **Low** `menu/menusys.c:79` (and :102) — `while(new_item_id != 0)` never ends for an unregistered page id (stale `boot_target`/action target) → UI task hangs holding `disp_lock`; panel + `/screenshot` freeze, no log.
8. **Low** `menu/setup_menu.c:95` — `setup_menu_remote_json` stops at <96 bytes left but a row can take ~102 → `n - p` wraps → closing `snprintf` overflows the 4096 buffer. Needs ~38+ long rows; not reachable today.

### 5 Shared widgets
_Reviewed 09-16 at HEAD `ae6681a`. Unverified — reviewer's severity, no verify pass._

1. **Med** `menu/cvmtx.c:297` — a preset whose `mxm` modes array is shorter than the current destination count forces the extra destinations to `CVM_OFFSET` (`mi` is NULL), contrary to the comment at 299-300. E.g. a Tape preset from before Reso loads with Reso's knob routed but inert, shown "+0%".
2. **Med** `menu/cvmtx.c:269-279` — `cvmtx_load` never resets first; only Tracker calls `cvmtx_reset_defaults` (`tracker.c:917`). A preset with no `mxs`/`msrc` keeps the previous preset's routing; destinations past a shorter array keep the old values. Loading A then old B carries A's routing into B, and autosave writes it into B.
3. **Low** `menu/cvmtx.c:103-104` — `rearm` read-then-clear in two steps on the audio task vs UI-task writes of `0xFFFF` (220, 224, 232) → a lost rearm; e.g. ABS source CV5→CV6 jumps straight to CV6's position instead of waiting for takeover.
4. **Low** `menu/wave_edit.c:94-96` — out-of-view beat ticks clamp to the strip edges → false grid/bar line in the first and last columns whenever a grid is set (also at file start when `grid_anchor > 0`).
5. **Low** `menu/sample_browser.c:170-173` — pressing an empty folder leaves `b.sel` unchanged but `list_refresh` reorders the rows → a different folder is highlighted; a second press goes somewhere else (e.g. empty `REC/` → back to POOL).

Checked clean: text_entry indices and bounds, caller lengths; wave_edit playhead height; sample_browser `k_fold` vs `SAMPLE_DIR_N`, modulo never zero.

### 6 Network + REST
_Pass 1 (09-16) reviewed only recent commits `d76adbb`, `4722ac9`, `26d724c`, not the whole files. Pass 2, a whole-file read, is below. Unverified._

1. **High** `rest-api/rest-api.c:1327` — `drop_sample_put_handler` `sprintf`s the `Name` header into two 32-byte stack buffers, unchecked. Since `4722ac9` the page sends up to 31 chars → names >15 overrun the JSN path buffer, >22 the filename buffer → httpd stack corruption on an ordinary web upload.
2. **Med** `rest-api/rest-api.c:2050` — `drop_ot_put_handler` still `char name[24]`; long `.ot` names are truncated → slice map saved under the wrong name, upload reports success, the slicer never pairs it.
3. **Low** `rest-api/rest-api.c:103` — sidecar cache `char id[24]`: put truncates to 23 chars, get compares the full id → always misses for names ≥24 → every `GET /files` re-reads the JSN of each long-named sample.

Pass 1 checked clean: API key no longer served; 8 web + 3 server sockets fit the 16 limit; `/files` index/delete/move/raw/peaks buffers fit 31 chars. Legacy `freesound/freesound.c` is only reached from Sampler2.

_Pass 2 (09-16), whole files: rest-api.c, rest-api.h, wifi.c, espnow_probe.c._

4. **High** `wifi/wifi.c:301` — `restartWifi` sets `wifi_ap_mode = 0`, but `ap_sta_retry_task` only falls back to the AP while it is set. A mistyped password saved via `POST /settings` → retries STA forever, never brings the AP back → unreachable until power cycle.
5. **High** `wifi/wifi.c:200-219` — `buildWifiConfig`: missing CONFIG.JSN/`settings` returns an uninitialised config, passed to `esp_wifi_set_config` inside `ESP_ERROR_CHECK` (can abort every boot); a missing `ssid`/`passwd` key dereferences NULL; unchecked `strcpy` into `ssid[32]`/`password[64]`, and `/settings` accepts any length.
6. **High** `rest-api/rest-api.c:2393-2395` — the OTA receive loop retries timeouts forever; a client vanishing mid-image → httpd worker hung, audio left muted, OTA handle open, REST gone until power cycle. Same loop in bootlogo (1572), mod_upload (2019), settings_post (1067), remote_params_post (1837). `drop_sample`/`drop_ot` already cap at 4.
7. **Med** `rest-api/rest-api.c:2115-2127` — `/trk/list` loop stops at `len >= 3936` but adds the untruncated `snprintf` return → a module name over ~150 chars pushes past 4096 → the final `snprintf` gets a huge size → heap overflow. Names are not JSON-escaped.
8. **Med** `rest-api/rest-api.c:2024` — `f_write` results ignored; on a full card the partial temp file is still renamed over the module (defeats the atomic write that exists to stop libxmp crashing). `drop_sample` (1399) returns 200 and starts the importer on a truncated take.
9. **Med** `rest-api/rest-api.c:1422` — `writeJSONFile(..., cJSON_Print(root))` never freed → every `PUT /drop_sample` leaks internal RAM; a NULL print isn't handled; the sidecar is written while the audio file is still open.
10. **Med** `rest-api/rest-api.c:1380` — unchecked `malloc(4096)` plus header buffers at 1334/1974/2053 and mod_upload (2015) → NULL deref under low internal RAM (e.g. Radio playing).
11. **Med** `rest-api/rest-api.c:1922-1933` — `machine_web_apply` (UI task) unregisters/registers URI handlers while the httpd task matches URIs against the same table (no locking in IDF 4.3) → a machine switch during 2 Hz polling can read a freed URI.
12. **Low** `rest-api/rest-api.c:497` — `stat()` without `sd_lock` in `peaks_handler`, `files_raw_handler` (513-517) and the rename sidecar step (468).
13. **Low** `rest-api/rest-api.c:2359-2366` — `/ice/state` returns the Icecast password in plain text (contradicts the apikey policy); 384-byte buffer truncates → invalid JSON; values are not escaped.
14. **Low** `wifi/wifi.c:244-265` — if boot fell back to the AP and the retry task later joins STA, `obtain_time()` never runs → no SNTP/TZ → wrong clock and certificate checks until reboot.

### 7 TFT library
_Reviewed 09-16: diff `b1489d3..HEAD` (our 3 commits). **No bugs found.**_

Checked: every write path updates the shadow (drawPixel / pushColorRep / send_data); grayscale copies taken before the buffer is rewritten; window bounds (y2 stop, off-panel clipped fallback, x2<x1, >320 px fills chunked); rotation keeps the same total size; the persistent fill buffer grows and compares the post-grayscale colour; glyph buffer reuse is safe because `disp_deselect` waits for DMA; `LB_SPI_DEVICE_NO_DUMMY` is set only on the display, whose reads run at 10 MHz or less.
Notes, not bugs: `drawPixel` updates the shadow before `disp_select()` succeeds (a failed select could leave one wrong pixel in the shadow); the glyph buffer is shared across tasks, which is safe while the SPI path serialises drawing.

### 8 Samplers A
_Reviewed 09-16, whole files. Unverified._

1. **High** `machine_slicer/slicer.c:416` — `slicer_start` passes `char first[1][24]` (via `slicer_list_samples(char out[][24])`) to `sample_list`, which writes 32-byte ids → first sample name ≥24 chars → stack corruption on switching to Slicer. (LFN leftover; builds with only a warning.)
2. **Med** `machine_slicer/slicer_priv.h:58` — `char pending[24]` not widened → names ≥24 truncated → "no sample", silent. Hits the browser and preset restore.
3. **Med** `machine_slicer/slicer_menu.c:460` — the Sensitivity screen switches to transient/Auto but leaves `ot_active` set; `recompute_slices` checks OT first → no effect on any sample with a `.OT`.
4. **Med** `machine_sampler3/sampler3.c:1264` — `s3_preset_load` sets `v->reverse` directly instead of `s3_set_reverse` → the reader never rebuilds the head; with the same sample already loaded the voice shows REV but plays forward.
5. **Med** `machine_sampler3/sampler3.c:515` — `s3_stop` waits ≤1 s then frees head/ring/loop-start cache without checking `s_alive`; the reader's rebuild loops don't check `s_run` → use-after-free on a slow card. (Slicer's stop already guards this by leaking instead.)

### 9 Samplers B
_Reviewed 09-16, whole files. Unverified. **Drums: nothing to report.**_

1. **High** `machine_instsampler/instsampler_priv.h:58` — `is_zone_t.sample` is still `char[24]`; `keys_load_zone_at` (`instsampler.c:148`) truncates to 23 → the sample plays now, but autosave/patches (650, 660) store the short name → silent or a zone short on the next boot/recall, no message. `keys_autotune` (289) loses the note hint (`..._C4`). Fix: `char sample[SAMPLE_ID_LEN]` like `drum_priv.h:65`.
2. **Med** `machine_instsampler/instsampler.c:89` (+ `:140`, `:194-206`) — zones are cleared while the audio task reads them; `keys_process` re-reads `z->frames`/`z->buf` per sample (566-576) → NULL read → PANIC. Hit by holding a note and pressing Clear Zones, Load Sample while a note sounds, or a `/remote/params` preset while playing. Fix: the `drum_clear_layer` order (zero frames, `vTaskDelay(1)`, then NULL buf) or per-block locals.

Notes: Drums pad labels `"%.8s"` (`drum_menu.c:177`, `:273`) are display-only, but similar long names look identical on the pads. `drum_stop`/`keys_stop` are safe (`machine_core.c:47-51` detaches and waits a tick).

### 10 Decks
_Reviewed 09-16, whole files. Unverified. No LFN leftovers (both use `SAMPLE_ID_LEN`)._

1. **High** `machine_deck/deck.c:657` — `stop()` never calls `bpm_analyze_abort()`; `start()`'s memset marks analysis idle while it runs → after switching away and back, loading Y starts a second analysis; the old one finishes, applies its BPM/grid to Y and writes Y's sidecar as current (never re-analysed). Two ~208 KB envelopes share global busy/abort flags.
2. **Med** `machine_deck/deck.c:661` — `stop()` frees the ring after a 1 s wait even if the reader is alive (blocked on `sd_lock`) → use-after-free; or on restart the old reader revives next to a new one and clears the alive flag so the next stop doesn't wait.
3. **Med** `machine_dualdeck/dualdeck.c:832` — same 1 s timeout; frees both rings regardless of the reader/analysis tasks.
4. **Med** `machine_dualdeck/dualdeck.c:307` — a queued analysis handoff clears the abort flag → an abort landing there is lost → the run outlives the machine and writes its sidecar under a zeroed name (`/sdcard/usr/.JSN`), clearing the new session's flags.
5. **Low** `machine_dualdeck/dualdeck.c:340` — reloading the deck currently being analysed queues nothing (only the other deck queues) → X's result is discarded, Y never gets a grid and can't loop until reloaded.

### 11 Tape + Looper
_Reviewed 09-16, whole files. Unverified._

1. **Med** `machine_tape/tape.c:606` — `tape_restore_last` reads `tapelast` into `char id[24]` (LFN leftover) → a 24–31-char name is saved in full but read back truncated → tape blank after a machine switch, no message.
2. **Med** `machine_tape/tape.c:386`, `:272` — punch and card-record modes test `trig_rising & 2` instead of `TP_RECBIT`; only momentary mode honours `rtr` → with play on TR2 and rec on TR1, one TR2 press both toggles play and punches record, and TR1 does nothing.
3. **Med** `machine_tape/tape.c:558` — `tape_set_len_sel` frees and reallocates blocks while `nblk`/`cap`/`len` still hold the old values; a TR edge in that window → audio task reads freed memory or writes through NULL → crash. Setup row or preset `lsel`.
4. **Med** `machine_tape/tape.c:585`, `:630` — a multi-second load checks "stopped" only at the start; a TR2 take mid-load writes the same memory, then the load sets `len`, `take_dirty=false`, `restore_id` → take discarded, rec flags stale. Crop's load-back (`tape_drop_adopt_kick`) has the same window.
5. **Med** `machine_tape/tape.c:171`, `:633` — `take_dirty` never cleared after a successful save → the punch-out autosaves CUT_0001, then `tape_stop` saves CUT_0002 and repoints `tapelast`; loading a sample does the same and blocks the UI. A duplicate file per machine switch.
6. **Low** `machine_tape/tape.c:177` — tape-full stop leaves `rec_stop_target` set (`tape_erase` doesn't clear it either) → the next take's quantized punch-out cuts immediately.
7. **Med** `machine_looper/looper_web.c:48` — the `/looper/save` worker reads `lp.tr[i].buf` but `looper_stop` doesn't wait for it → machine switch during a long save = use-after-free.
8. **Low** `machine_looper/looper.c:307` — panel and web saves both do `sample_next_index("LOOP")` + `fopen` unlocked → same `LOOPnnnn.WAV`, mixed/corrupt file.
9. **Low** `machine_looper/looper.c:69` — a failed alloc of track k returns without freeing 0..k-1; `machine_activate` doesn't call stop and the next start memsets the pointers → up to 3 × 706 KB PSRAM leaked per attempt.

### 12 Synth-type
_Reviewed 09-16, whole files (libxmp skipped). Unverified._

1. **High** `machine_tracker/tracker.c:680` — `tracker_stop` waits 2 s for the render task, but module load never checks for shutdown (~5 s for 2 MB) → switching back mid-load starts a second render task on the shared static libxmp context; the old one frees it under the new one → panic. Orphaned 32 KB stack + PSRAM module copy even without switching back.
2. **Med** `machine_synth/synth_priv.h:55` — `sy.wave_name` still `char[24]` (LFN leftover) → a 24–31-char wave id is saved truncated → silently falls back to saw/square on the next boot.
3. **Low** `machine_granular/granular.c:218` — `granular_load` clears grains and overwrites the buffer without waiting a block; the old length still stands → grains play into the new sample at old positions (audible burst).
4. **Low** `machine_synth/synth.c:46` — the wavetable is overwritten while playing and the length updates only at the end → noise for the whole card read during a held note.
5. **Low** `machine_glitch/glitch.c:57` — partial PSRAM allocation failure isn't freed on start error → a leak per retry.
6. **Low** `machine_glitch/glitch.c:168` — preset `division` is not range-checked before use as a shift (Setup clamps 0–3, preset load doesn't) → garbage window length from a remote post or hand-edited autosave.

Checked OK: Tracker filenames 40 chars; its 24-char slots are libxmp instrument names, not sample ids.

### 13 Streaming + editing
_Reviewed 09-16, whole files. Unverified. Clean: no LFN leftovers (`RADIO_NAME_LEN` 24 holds station names, `editor_menu.c:122` holds time strings); no socket leaks after `esp_http_client_init`; FS pipeline/search handle `xTaskCreate` failure._

1. **Med** `machine_freesound/fs_menu.c:371` — pressing a result that isn't downloaded calls `fs_audition()` first; the open fails but the player (reader task, internal RAM) isn't destroyed → `fs_get_start` can't get its 40 KB block → "no RAM for the download task" on the first press of every new sound. Same leak at `fs_menu.c:437` (file missing).
2. **Med** `machine_editor/editor.c:172` (+270, 346, 441, 519) — `s_running`/`s_scanning` are set inside the new task, which runs at prio 4 below httpd's 5 → a double POST `/edit/apply` or web + panel both pass the check → two jobs write the same `usr/XX_NNNN.WAV`. Also `clip_task` sets `s_cjob=-1` after `s_running=false` (512-513) → a `start_clip` in between gets wiped and reports DONE with nothing written.
3. **Med** `machine_freesound/fs_machine.c:245` — `r == 0` before `content_length` just breaks (a dropped connection/timeout) → the partial body is converted and reported "installed". The streaming fallback (291) has no length check at all.
4. **Med** `machine_freesound/fs_menu.c:367-371` — the sanitised title is the pool id and "in pool" is tested only by trying to audition it → two results with the same title, or an existing pool sample with that name, plays the other file and never downloads the chosen one.
5. **Low** `machine_editor/editor.c:548`, `:376`; `machine_freesound/fs_machine.c:648`, `:695` — `sampplay_destroy(s_play)` runs before `s_play` is cleared; `process()` keeps rendering it → brief use-after-free. Fix order: copy pointer, NULL it, wait a tick, destroy. (Machine-local, separate from shared 3.7.)
6. **Low** `machine_radio/radio.c:416` — `radio_start` forces `s_ntasks=0`; a stream task that outlived the 7 s stop wait later decrements it to −1 → the next stop doesn't wait → `radio_stop` frees `rd.ring`/`s_rs.ext` under the live task.
7. **Low** `machine_freesound/fs_machine.c:272/296/298` — on "SD write failed"/"mp3 too large" the partial `usr/<name>.mp3` is left; the next `POST /import` scan converts the fragment.
8. **Low** `machine_editor/editor.c:128-140, 155, 76-80` — short reads break silently, `fwrite` unchecked → a full card ends `ED_DONE` "wrote XX_NNNN" with a truncated take.
9. **Low** `machine_editor/editor.c:465-468` — Cut over the default range (0..frames) writes a zero-frame WAV that probing rejects → junk file in `usr/`.
10. **Low** `machine_freesound/fs_machine.c:83-84, 549` — pipeline, search task and the `/fs/search` proxy share static `s_http_status`/`s_http_err`; `start_job` ignores `s_searching` → wrong error reported, two TLS sessions compete for internal RAM.
11. **Low** `machine_freesound/fs_machine.c:108-121` — PSRAM body alloc failure after headers returns −1 with status still 200 → error shows "freesound HTTP 200".
12. **Low** `machine_freesound/fs_machine.c:369-370` — `fsm.busy` check-and-set not atomic; panel + `/fs/get` can both start a pipeline (two 40 KB stacks; progress fields shared).

## Triage
_09-16. **92 findings, none verified** — every one came from reading code, nothing was built, run or reproduced. The "Fix" order below is by consequence *if real*; each fix starts by confirming the finding (by reading the code again or on the bench) before changing code. Ids are `chunk.finding`._

Checked against memory: **4.7** (menusys unregistered page id spin) was already noted as a latent landmine in the panel-first FS/Editor note, "noticed, not proven to have fired", so it isn't new. Nothing else overlaps the existing notes. The reverb pop, FX combo dirt and HTTP burst investigations were not touched by any finding, and **6.x** doesn't reopen the lwIP socket pool (8 + 3 sockets fit the 16-socket limit).

### Themes (duplicates merged)
Most findings are a few mistakes repeated. Fixing by theme is cheaper than finding by finding.

- **A. Long-filename leftovers (24 → 32).** `4722ac9` widened `SAMPLE_ID_LEN` but missed hand-sized buffers: **3.1, 3.2, 6.1, 6.2, 6.3, 8.1, 8.2, 9.1, 11.1, 12.2**. One mechanical sweep: grep for `[24]` / `24)` near sample ids and use `SAMPLE_ID_LEN`. It has the best ratio of payoff to effort in the whole list, and three of these corrupt memory (3.1, 6.1, 8.1).
- **B. Teardown: "wait N s, then free anyway" / no tick before clearing.** **3.7, 3.8, 8.5, 10.2, 10.3, 11.7, 12.1, 13.5, 13.6**, plus the audio-vs-UI clears **9.2, 11.3, 12.3, 12.4**. There's prior art to grow: Slicer's stop logs and leaks instead of freeing, and `drum_clear_layer` zeroes the length, waits a tick, then frees. That calls for one shared helper, not nine local fixes.
- **C. Failed start leaves nothing running.** **1.1, 1.2** (no Stub fallback), **11.9, 12.5** (leaks on a partial allocation).
- **D. BPM analysis lifecycle.** **10.1, 10.4, 10.5**. The analyser's global busy/abort flags outlive the machine.
- **E. Config / Settings / WiFi.** **4.1, 4.2, 4.3, 3.11, 6.4, 6.5, 6.14**.
- **F. Web server robustness.** **6.6** (unbounded receive retry ×5 handlers), **6.7, 6.8, 6.9, 6.10, 6.11, 6.12, 6.13**.
- **G. Import / download / write integrity.** **3.3, 3.4, 3.5, 3.6, 3.9, 3.10, 13.3, 13.7, 13.8, 13.9**. The unchecked write/short read pattern repeats in 6.8 and 13.8.
- **H. Concurrency guards (check-then-set).** **1.7, 3.4, 11.8, 13.2, 13.10, 13.12, 5.3**.
- **I. Preset load doesn't fully restore state.** **5.1, 5.2** (cvmtx), **2.2, 2.3** (fxrack), **4.4** (autosave defaults on switch), **8.4, 12.6**.
- **J. Recording / bounce state.** **1.3, 1.4, 1.5, 1.6**.
- **K. DSP / timing.** **2.1, 2.4, 2.5, 1.10**.
- **L. Machine behaviour / UI.** **8.3, 11.2, 11.4, 11.5, 11.6, 13.1, 13.4, 13.11, 4.5, 4.6, 5.4, 5.5, 1.8, 1.9**.

### Fix — tier 1: crash, hang or memory corruption reachable in normal use

> **Theme A: FIXED in the working tree 09-16, NOT committed, NOT flashed.** Each finding was confirmed by reading the code first; all 10 were real. Changes:
> - Every hand-sized `[24]` name buffer is now `SAMPLE_ID_LEN`: `sample_list_recent.c` (alloc + 3 memcpy), rest-api sidecar cache `id`, `drop_ot` `name`, slicer `pending` + `first[1]` + `slicer_list_samples` signature, Keys `is_zone_t.sample`, Synth `wave_name`, Tape `tape_restore_last` `id`.
> - `drop_sample_put_handler`: path buffers 32 → 64 and `snprintf`; a name of `SAMPLE_ID_LEN` or longer is refused with 400 instead of truncated. `drop_ot` refuses an over-long name the same way (it had silently truncated) and now NULL-checks its header buffer.
> - The Synth header tag now shows the wave name with `%.24s`: the same on-screen cap, and it silences IDF 4.3's `-Werror=format-truncation`.
> - Display-only `[24]` buffers (status and bpm strings, Editor time strings, Radio station names) were checked and left alone.
> - IDF 4.3 build is clean. Still to check on the unit: upload a 24–31-char name from the web, switch to Slicer with a long first sample, reboot with a long name loaded in Keys/Synth/Tape, and open a dated browser.
1. **Theme A sweep.** It covers 3.1, 6.1 and 8.1, which corrupt memory, and the silent truncations too.
2. **9.2**: Keys crashes on Clear Zones or Load Sample while a note is held.
3. **12.1**: switching back to Tracker during a long module load.
4. **Theme B helper**, applied to 3.7/3.8, 8.5, 10.2/10.3, 11.3, 11.7, 13.5, 13.6.
5. **1.1 / 1.2**: fall back to Stub when a machine fails to start.
6. **4.1**: crash when leaving Settings after a failed config read.
7. **6.6**: a stalled OTA or upload hangs httpd with audio muted. **6.4**: a bad WiFi password leaves the unit unreachable.
8. **3.6**: a corrupt WAV hangs the SD bus under `sd_lock`.
9. **6.5, 6.7, 6.10, 6.11, 2.4, 12.3/12.4**: plausible, but they need unusual input or low RAM.

### Fix — tier 2: silent data loss or wrong saved state
**3.3** (import deletes the source) · **5.2** (routing carried between presets) · **4.2** (Settings rolls back web changes) · **4.4** (autosave writes defaults on a slow switch) · **2.2/2.3** (FX settings lost, `dlyt:0`) · **11.4, 11.5** (Tape discards a take / duplicate CUT files) · **13.3, 13.7, 6.8, 13.8** (truncated files reported as success) · **10.1, 10.4** · **1.4, 1.7** (recording refused until reboot).

### Fix — tier 3: wrong audio or behaviour
**2.1** (filter blows up; high if real, worth an ear test first) · **11.2** (Tape punch ignores the TR setting) · **8.3** (Slicer Sensitivity inert in OT mode) · **8.4** (preset REV plays forward) · **13.1, 13.4** (Freesound first press / same-name results) · **13.2** · **3.5, 3.9, 3.10** (MP3 with ID3, 8-bit AIFF, WAVE_EXTENSIBLE rejected) · **1.3, 1.6** · **5.1** · **2.5, 1.10** · **10.5**.

### Watch: low consequence or not reachable today
**4.8** (Setup JSON overflow needs 38+ long rows) · **4.7** (already known, never observed) · **1.8, 1.9, 4.5, 4.6, 5.3, 5.4, 5.5, 6.3, 6.12, 6.13, 6.14, 3.11, 11.6, 11.8, 11.9, 12.5, 12.6, 13.9–13.12**.

### Not a bug
None yet. That call needs verification, and none has been done.
