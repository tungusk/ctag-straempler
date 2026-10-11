# Review of the review fixes — 2026-10-11

Scope: every commit `8750c96..e95d8c4` (the 09-30 themes A–Z, menu/display, the bench
findings, the 10-10 follow-up, and the other session's `1428356` ptab + `d5bf2d5`
hostsim fixes). A REGRESSION review: did a fix break something, miss a path, or not
fix its finding?

Method (Arlo's review shape): Sonnet finders per group read the diffs and the code
around them → a full-model verifier per group tries to refute each finding (against
the code, `CLAUDE.md` and `plans/code-review-2026-09-29.*`) → triage here → STOP for
Arlo. All agents read-only, none contacted the unit.

Groups: G1 audio/DSP/FX · G2 Tape/Looper/Sampler/recording (+ ptab, hostsim) ·
G3 decks/readers/per-machine · G4 core/files/config/import/REST · G5 web page.

## Status

- [x] G1 finder (2 minor) · [x] G1 verifier: G1-1 confirmed minor (pre-existing quantisation; snap hz so stats are honest), G1-2 REFUTED (no reachable caller); own pass over fedaae1 + 3a045c3 clean
- [x] G2 finder (9: 2 serious) · [x] G2 verifier: G2-2 CONFIRMED (Sampler regression), G2-1 CONFIRMED, G2-9 bounce part CONFIRMED; G2-3/4/6/7/8 confirmed low; G2-5 plausible
- [x] G3 finder (5: 1 serious) · [x] G3 verifier: G3-1 CONFIRMED (worse: the whole stream task ends; ICY gives 200, so the hs!=0 exception never fires; superseded task can set_err over a new station), G3-2/3/5 CONFIRMED, G3-4 plausible-low
- [x] G4 finder (7: 2 serious) · [x] G4 verifier: G4-1 CONFIRMED (power cut deletes the good CONFIG backup), G4-2 CONFIRMED low, G4-4 CONFIRMED for Drums/Granular/Synth (+Slicer race); G4-3 plausible low, G4-5/6 confirmed low, G4-7 impact REFUTED; lock order display→config→SD holds
- [x] G5 finder (1 broken, 1 wrong, 5 minor) · [x] G5 verifier: G5-1 CONFIRMED (DoubleDecker only), G5-2 plausible (race, self-recovers), G5-3/5/6/7 confirmed low, G5-4 plausible low; NEW: Radio sel resets on an empty Apply, Keys flat smp edit wipes zones, clk_src re-posted on every Apply (older)

## Confirmed findings

Ranked by how much it hurts. "Mine" means one of my 09-30/10-10 fixes caused it.
Fix details are in the verifier notes below.

### Tier 1: regressions that break playing (mine)
1. **G2-2 Sampler goes silent on short samples after a seek.** `sampler3.c:585` (#58 fix) rejects every ring frame once `ring_lo` has moved past the head. One-line fix.
2. **G3-1 Radio stream dies on one slow header.** `radio.c:313`: status -1 (timeout) is treated as fatal and ends the stream task instead of reconnecting. An old superseded task can also put its error over a new station.
3. **G5-1 DoubleDecker web Apply.** The diff drops `cvv`/`lq`, so CV-matrix edits are ignored and loop length comes out ×4.
4. **G5-V1 Radio station resets on an empty Apply.** The page posts `{}` and the firmware sets `sel = 0`. Fix both ends.
5. **G5-V2 Keys flat `smp` edit wipes every zone.** It now posts alone and runs keys_load_zone.

### Tier 2: data loss
6. **G4-1 Power cut during a CONFIG.JSN rewrite deletes the good backup.** Boot sees no CONFIG.JSN, writes defaults, and that write removes the .JBK (mine, #20 fix). Recover .JBK → .JSN at boot, and only remove the backup when a live file existed.
7. **G2-1 Looper web save overlapping a panel save or bounce gives a torn WAV.** Plain shared busy byte. One mutex, 409 when busy.
8. **G2-9 Switching away from Sampler ends a running web /bounce.**

### Tier 3: wrong behaviour you'd notice
9. **G4-4 Web "load to machine" leaves Drums, Granular and Synth screens stale** (old name, waveform, header) until re-entry. Slicer has a small race. Per-machine signature checks, like Deck's.
10. **G3-2 Slicer's "ignore .OT" sticks to the next sample.**
11. **G5-V3 Every web Apply re-posts the clock source**, undoing a panel clock change (older).
12. **G2-4 Editor web load skips the audition stop and view reset.**
13. **G5-2 The first Apply after page load can say "machine changed"** and drop the edits (self-recovers).
14. **G4-5 Tape/Editor web load don't URL-decode** (`PNOF#3` fails). Tape also loads from a buffer the web can overwrite mid-load (= G2-5).
15. **G4-6 Upload-tab re-upload of a usr/ WAV leaves the WAV orphaned** behind the new RAW.

### Tier 4: low, cheap
- G2-3 OTA: re-check `recording` before switching partitions · G2-6 Looper: an empty track can be armed · G2-7 Tape still clamps ids at 9999 · G2-8 refused Cut has already replaced the clipboard · G3-3 failed Drums layer can't be retried from the panel · G3-5 Keys unloaded-zone list race (broader: GET /remote/params runs preset_save on httpd) · G4-2 import kick lost / double importer under low RAM · G4-3 the boot sweep matches any `*.BAK` · G5-3/4/5/7 page sanitiser, fetch timeout, "queued" wording, rename box wiped · G1-1 probe rate quantised · G3-4 Deck resume after a large move (your call).

### Refuted or not worth it
G1-2 (no reachable caller) · G4-7 (`/files` doesn't use that path) · G5-6 (by design).

### Design question for Arlo
**Tape quiet-pause save with a crop active** saves only the crop window, adopts it and marks the take clean, so anything outside the crop is gone on restore. Leave/load already did this; the 10 s save now makes it happen early in a session. Keep it, or have the quiet save write the whole take and leave the crop for an explicit leave/load?

## Raw finder output

### G1 (audio/DSP/FX) — no crash/audio regressions; 2 minor
- G1-1 minor `wifi/espnow_probe.c` probe_task (6678b95): delay `pdMS_TO_TICKS(1000/hz)` floored at 1 tick quantises the rate to 100/n Hz (51..99 all send at 100). Spin is fixed.
- G1-2 wrong-behaviour `audio/audio.c` audio_remote_trig (6678b95): the 2-tick floor also delays the page's ms=5 RELEASE by 10-20 ms; a fresh trig inside that window may lose its edge.
- Clean per the finder: 2943663, 3a045c3, 00116dc, 23ce22b, fedaae1, 582a4c4, 1069aa3, rest of 6678b95.

### G5 (web page)
- G5-1 BROKEN `index.html` rpApply diff (716553e): DoubleDecker's preset_load needs `cvv>=1` for the matrix and `lq>=1` for llen units in the SAME body; the diff drops them → matrix edits silently ignored, llen ×4.
- G5-2 wrong `index.html` pollStatus/rpApply (716553e): rpMach set from curMach before the first poll ('' ) → the first Apply after page load says "machine changed" and reloads (edits lost); `pm&&` also skips a reload on ''→B.
- G5-3 minor: rename/batch sanitisers strip '-' though the device accepts it.
- G5-4 minor: stBusy wedges polling while one /status hangs (no fetch timeout).
- G5-5 minor: load-to-machine says success when the load was only queued (Keys arena full, Tape rolling).
- G5-6 minor: Keys add-zone reloads every zone (accepted cost).
- G5-7 minor: a machine switch re-renders files and wipes an open rename box.

### G2 (Tape/Looper/Sampler/recording)
- G2-1 data-loss `looper.c` busy_mask (511b9f6): plain shared byte; web save + panel save + bounce overlap → bounce memcpys into a track being saved, holds released by the other party.
- G2-2 wrong-audio `sampler3.c:585` (cce4dab): `if (resident) return v->ring_lo == v->head_frames;` → after a seek past the head on a short (≤5 s) sample every ring frame is rejected; the voice starves and self-heal re-seeks into the same state. (My #58 fix — likely regression.)
- G2-3 minor `rest-api.c` OTA: recording checked only at the start of a tens-of-seconds upload.
- G2-4 wrong `editor_web.c` /edit/load: skips the panel's audition stop + view reset.
- G2-5 minor `tape_web.c`: web_load name written unsynchronised; refuse while a request is pending.
- G2-6 minor `looper.c`: a track with no buffer can be ARMED, then silently falls to EMPTY; 5×100 ms retry per missing track stalls the switch.
- G2-7 minor `tape.c`: Tape still clamps its save id at 9999 (CUT_9999 overwritten).
- G2-8 minor `tape.c`: a refused Cut has already replaced the clipboard; "no room" sticks.
- G2-9 wrong `sampler3.c` s3_stop: finishes ANY active recording incl. a web /bounce; leftover rec_load_pending can auto-load later.

### G4 (core/files/config/import/REST)
- G4-1 data-loss `fileio.c` checkSDStructure (f87f055): a power cut between the two renames leaves CONFIG.JSN missing + CONFIG.JBK good; boot's `stat == -1 → createConfigFile()` runs first, writes defaults and its writeJSONFile `remove(bak)` deletes the good backup. The fix misses its own case.
- G4-2 wrong `sampimport.c` (842d236/437f67d): busy/again not atomic — a kick between the last `again` read and `busy=false` is lost; esp_timer retry + httpd kick can start two importer tasks.
- G4-3 data-loss minor `fileio.c` sweepDir: matches every `*.BAK`, deletes a user's `X.WAV.BAK` or renames `FOO.BAK` to `FOO`.
- G4-4 wrong `menu.c` (d69c3ed + 3975965): no re-enter on the Live/boot page after EV_REMOTE_PRESET → a web 'load to machine' may not repaint the Live page's name/waveform.
- G4-5 minor `tape_web.c`/`editor_web.c`: names read undecoded; Tape's tape_load holds a pointer into tp.web_load during a multi-second load.
- G4-6 minor `rest-api.c` drop_sample: a native .RAW re-upload of an id that is X.WAV in usr/ shadows the WAV.
- G4-7 minor perf `fileio.c`: every missing sidecar now costs two extra stats (+ sd_lock) for the .JBK fallback.

### G3 (decks/readers/per-machine)
- G3-1 wrong `radio.c:313` (3bed9ce): IDF 4.3 status is -1 when headers time out / the server closes; `hs != 0 && hs != 200` makes that a permanent 'HTTP -1' stop instead of the old reconnect. Same after a failed reopen in the redirect loop.
- G3-2 wrong `slicer.c` (49ebd64): ot_off survives into another sample (preset without "otoff", web load, preset naming another sample) → its .OT ignored.
- G3-3 minor `drum_menu.c:865`: a kept-name failed layer can't be retried from the panel browser (same-name test ignores len 0).
- G3-4 minor `deck.c` deck_toggle_play (8b71683): `to` computed before folding the pending move; a large move still resumes at the new loop start.
- G3-5 minor race `instsampler.c`: s_unloaded_zones read by preset_save (UI/httpd) while load/clear modify it.
- Clean per the finder: ff05e05, the Deck/DD commit check + portMUX, dk_ge/gt, Keys 'same' path, Tracker zero-row orders, Editor, clamps.

#### G2 verifier notes
- G2-2 fix: `if (f < v->ring_lo) return false; if (resident) return true;` — the [ring_lo, wpos) window is at most one ring long, so it is always valid; optionally guard the reverse wrap-to-end (:1014) with `ring_lo == head_frames`.
- G2-1 fix: one mutex serialising looper_save_track and looper_bounce (take with timeout 0 → -4 BUSY / 409).
- G2-9 fix: `if (recording_is_active() && !audio_bounce_active()) recording_finish();`
- **Design point for Arlo (73cba38):** the quiet-pause save follows the leave/load rule — with an active crop it saves only the crop window (TLP_), adopts it and marks the take clean, so material outside the crop is not kept on restore. Pre-existing semantics, but now fires 10 s into a session.

#### G3 verifier notes
- G3-1 fix: `hs < 0` → close/cleanup and go through the reconnect backoff (`continue`, attempts>8 → 'no response'); `hs != 200` → error only if `gen == s_gen`.
- G3-2 fix: tie ot_off to a sample name (`ot_off_for`), or at minimum reset ot_off when a preset without "otoff" loads.
- G3-3 fix: panel browser loads when `!L->len || strcmp(...)`.
- G3-5 fix: a mutex around s_unloaded_zones; preset_save duplicates under the lock. Broader: every machine's preset_save runs on httpd for GET /remote/params while the UI task mutates state — marshalling that GET onto the UI task would fix the class.
- G3-4: fold first, then compute `to` with a signed modulo; Arlo's call whether it matters.

#### G5 verifier notes
- G5-1 fix (page): send `cvv` with the matrix and `lq` with `llen` when present in rpBase. Keep the firmware gate (it rejects old autosaves).
- G5-2 fix: `if(curMach!==pm&&(pm||(rpData&&rpMach!==curMach)))rpLoad();`
- G5-V1 NEW regression: an Apply with nothing changed posts `{}`; radio_preset_load does `rd.sel = 0` first → station resets. Fix page (skip empty) + firmware (`if(!node){rd.sel=0;return;}`).
- G5-V2 NEW behaviour change: on Keys the flat zone-0 keys now post alone; an ADVANCED edit of `smp` runs keys_load_zone, clearing every zone. Send `zones` whenever a flat key changes (or hide the flat keys).
- G5-V3 older: the glob picks (clk_src) are re-posted on every Apply, undoing a panel clock change. Diff them against rpBase.imap.
- G5-3: allow '-' in the three sanitisers. G5-4: AbortController 3 s. G5-5: say 'queued'. G5-7: skip renderFiles while libEdit.

#### G4 verifier notes
- G4-1: `fileio.c:168` short-circuits on the stat, so validateConfig's .JBK fallback never runs; `writeJSONFile` (:412) does `remove(bak)` before checking `had`. Fix: `json_recover()` for CONFIG.JSN and AUTOSAVE.JSN at boot (rename .JBK over a missing or zero-length live file), and `if (had) remove(bak);`. AUTOSAVE is mostly safe (read paths fall back), except when the backup read fails and `{}` gets written.
- G4-2: portMUX around the test-and-set in samp_import_start and around the again/busy hand-off at the end of import_task.
- G4-4: Drums: per-pad signature (layered + layer names) → draw_pad_cell + build_ring. Granular: name + gr.len → gran_draw_wave. Synth: hash wave_name into osc_sig → draw_header. Slicer: `sl.load_gen++` before `loading = false`. Sampler/Deck/DD/Keys already repaint.
- G4-3: require `X.<WAV|AIF|AIFF|RAW|MP3>.BAK`.
- G4-5: url-decode like fs_web.c/radio_web.c; tape_web_load_service copies into a local and clears the flag before tape_load.
- G4-6: remove the usr/ WAV/AIF twin when the RAW upload closes OK (or 409).
- Lock order display → config → SD and import-mutex-after-sd_lock: no counter-example (12 configSet* call sites not all checked).
