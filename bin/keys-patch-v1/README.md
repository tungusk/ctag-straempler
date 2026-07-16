# keys-patch-v1 — 2026-07-16

Full firmware snapshot, **OTA-capable layout** (two 3 MB app slots + otadata,
rollback-enabled bootloader). `./flash.sh [PORT]` serial-flashes it — the
**recovery path** if a bad OTA ever fails to boot. After this, updates are
WiFi: `tools/ota.sh`.

## Highlights since keys-v1

- **Shared `components/util/preset_store.{h,c}`** — the Synth #23 named-patch
  mint/write/list/read code factored into one util (`{dir, pfx}`-parameterized,
  `<PFX>NNN.jsn`, 8.3-safe ids, sd_lock'd, UI-context only). Synth now calls it
  through thin wrappers; on-disk format unchanged (`usr/synth/PAT_NNN.jsn`).
- **Keys named patch save/load** — Setup rows "Save Patch" / "Load Patch"
  (newest-first browser), files in `usr/keys/PAT_NNN.jsn`, the exact #23
  grammar. Load re-arms knob takeover and reloads the zone sample.

Smoke-tested on hardware via teleremote: save → corrupt a param → load
restored it, on BOTH Keys and Synth.
