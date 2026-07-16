# menu-unify-v1 — 2026-07-16

Full firmware snapshot, **OTA-capable layout** (two 3 MB app slots + otadata,
rollback-enabled bootloader). `./flash.sh [PORT]` serial-flashes it — the
**recovery path** if a bad OTA ever fails to boot. After this, updates are
WiFi: `tools/ota.sh`.

## Highlights since slicer-fx-v1

- **Shared Setup-menu framework rolled out to ALL machines** (`components/menu/
  setup_menu.{h,c}`). Every machine's Setup page now follows one house grammar:
  - **Small option sets** (on/off, modes, engine, filter type) → **press cycles**.
  - **Ranges** (numeric / % / ms / ppb) → **press enters `[ ]` edit, turn adjusts**;
    the value sits at a fixed right position (1-char pad) so it never shifts when
    selected — brackets hug it directly.
  - **Sub-pages** (Sample / Track / Module loaders, CV Matrix, Sensitivity,
    Record, Bounce) → **press opens them**.
- Migrated machines: **deck, dualdeck, drumsampler, granular, glitch, looper,
  sampler3, slicer, tracker** (synth was the pilot). Each machine's hand-rolled
  `setup_redraw` + pos/sel handler collapsed to a one-line delegate to
  `setup_menu_event()`, reusing its existing render/adjust logic as callbacks.
- Preserved behaviours: deck still polls auto-analysis on the slow timer before
  delegating; drums keeps its dynamic Sel-CV row hiding via a rebuilt
  visible-items buffer.
- Intended UX change: mode-cycle rows now cycle **forward on press** instead of
  press-to-edit-then-turn. Cosmetic loss (accepted): per-machine Setup footer
  hint lines and live-updating Analyze % on deck's *Setup* page are gone (the
  framework has no footer / timer redraw hook; deck's *Live* page still shows
  live analyze %).

Build + `tools/proof_build.sh` both pass. OTA-flashed to the module (ota_1),
boots clean on Synth. Not yet full-hardware eye-tested per machine.

## Recovery / layout note
OTA layout: bootloader `0x1000` / partition-table `0x8000` / ota_data `0xf000`
/ app `0x20000`. `bin/synth-v1` is the OLD single-app layout (app @ 0x10000) —
flashing it reverts OTA; re-migrate to get OTA back.

Matching git tag: `menu-unify-v1-20260716`.
