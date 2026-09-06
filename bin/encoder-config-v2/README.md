# encoder-config-v2-20260829

Serial-flash archive of `encoder-config-v2-20260829` (commit `c39bb49`,
built with the CLEAN tree — 10 dBm PHY caps, NO antenna-test changes).
This is the build running VERIFIED on the NEW unit (192.168.3.85) since
2026-08-29; unit 1 deliberately stays on `keys-multisample-v1`.

Contains vs keys-multisample-v1:
- `settings.encres` (2/4) + `settings.encdir` (0/1) — per-unit encoder
  resolution/direction, read at boot, live via POST /settings. The new
  unit's card carries encres 4 + encdir 1 (EAR-CONFIRMED by Arlo).
- Everything the stale keys-multisample-v1 ARCHIVE was missing: the
  Freesound stack fix + /sysinfo uptime/reset (`552e73e`), reverb
  wet-step trap (`0e09210`), keys zones-before-FX preset fix (`c94a9cf`),
  `GET /tftread` readback probe (BUILT, NEVER RUN — SJ1 still open).

Ear debt: none new (encoder feel confirmed). /tftread bridge test parked.
Flash: `./flash.sh <port>` (default port is the main bench's CP2102).
