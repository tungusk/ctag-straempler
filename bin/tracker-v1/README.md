# tracker-v1

Full firmware snapshot at the tracker de-hub + Live redesign milestone
(`v09-machines`, commit `5ce91e2`, tag `tracker-v1-20260709`).

Flash: `./flash.sh [PORT]` (default port `/dev/cu.usbserial-3110`,
`--flash_size detect`).

## What's in this build (vs deck-v1 / drums-remote-v1)

The **Tracker** machine (libxmp player, ~50 module formats: MOD/XM/IT/S3M/669…)
is now a first-class, hub-less machine and got a hardware-tuned Live page:

- **Hub-less navigation** — boots straight into Live; long-press toggles
  Live↔Setup; on Setup, scroll past the list to the top-right **System**
  affordance → machine selector. (No more getting stuck in the old menu bar
  while a module plays.)
- **Live page** — machine header, big module **title**, full untruncated
  module **type** + channel count, status line (state / pattern / module BPM),
  a slim transport bar, and the **message panel**.
- **Message panel** — the module's sample/instrument **name slots** (where
  composers traditionally spell out the song's message/credits), listed in a
  small font and **scrolled by knob7 / CV7**. Instrument names are preferred
  (XM/IT); sample names are the fallback (MOD).
- **Setup** — Module (browser), Loop, Sound (Amiga/Clean), Sync, Clock Src,
  Clock (ppb), and **Info Text** (ON/OFF; blanks the message panel). Settings
  persist per-machine in AUTOSAVE.JSN.

Modules live in `usr/MODS/` on the SD card; upload/list/download/delete over
WiFi via the core `/trk/*` REST endpoints (work regardless of active machine).

Also includes the rest of the current `v09-machines` state: the five
hub-less machines (Deck, Looper, Slicer, Granular, Glitch), the consolidated
single **Sampler** (old engine hidden), Drums, Freesound, and teleremote.

## Known-not-done

- **Tempo sync** — the Sync toggle + external-clock tempo-factor path exist,
  but clock detection isn't reliable yet, so the sync status line is hidden
  from Live. Re-enable once detection is solid.
