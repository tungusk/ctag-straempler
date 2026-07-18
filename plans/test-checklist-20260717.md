# Ear / Eye test checklist — 2026-07-17 (post FX-pack marathon)

Everything below was BUILT + FLASHED this session but is largely unverified by
ear/eye (a lot of building, little testing). Work top-down: the **OPEN BUGS** are
known-broken; the rest is "confirm it's right". Load meter for CPU tests:
`curl http://<ip>/status` → `"aus"` (µs/block, **1450 = 100 %** of budget).

## 🔴 OPEN BUGS (known issues — confirm + they still need a fix)

- [ ] **Flanger rings up on a sustained note** (EAR, Synth). Metallic harshness
      builds over a held tone. Damping fix + default feedback 0.4→0.25 applied but
      UNVERIFIED. To get the new 25 % default, toggle Flanger OFF then ON once
      (old 0.4 may reload from autosave). Did the ring-up go away?
- [ ] **Flanger on a NON-synth machine** (EAR, Keys + Tape) — Arlo wanted it
      checked off Synth; does it behave the same / better?
- [ ] **Reverb SHIMMER feedback escalates on silence** (EAR, any machine). The
      octave-up feedback builds up when nothing is playing. Confirm; it's a real
      bug in util/reverb (predates FX pack), fix pending.
- [ ] **Tremolo** (EAR) — does it have its OWN artifact, or was it only pulsing
      the flanger's harshness? Test tremolo alone on a held note.

## 🔊 EAR tests — the FX pack

- [ ] **Delay** on Synth / Tape / Drums (Keys already ✓). Tape delay is
      CLOCK-SYNCED — set a BPM (or feed a clock) and confirm the "Dly Div"
      divisions (1/16…1/2) lock to tempo.
- [ ] **Overdrive** on Keys/Synth/Tape — drive/tone/bias/level sound right. AND:
      the `tanhf`→fast-approx swap — does it sound the same as a true tanh? (subtle)
- [ ] **Tremolo** on Keys/Synth/Tape — sine/tri/sqr shapes; Stereo = auto-pan.
- [ ] **Reverb** modes (Room/Hall/Plate/Shimmer) still correct after the FX-page
      refactor (aside from the Shimmer bug above).
- [ ] **CPU headroom per machine** — with the FX you like stacked, watch `aus`.
      Synth is the heaviest (~500 µs idle, ~700 µs all-5); Keys/Tape should be
      roomier. Find the comfortable FX count per machine. Crackle = out of slack.
- [ ] **Pitch-freeze on note release** (EAR, Keys + Synth) — a released note's
      tail holds its own pitch (no C3 blip). (The reported "C3 tail" was actually
      loop-mode, but confirm the freeze didn't hurt normal release.)

## 👁 EYE tests — UI

- [ ] **FX submenu navigation** (Keys/Synth/Tape): Setup → **FX >** opens the FX
      page; every row shows a sane value; turning adjusts; the **Setup** top
      affordance returns; long-press → Live. The FX row shows "N on >".
- [ ] **Shortened Setup pages** render/scroll fine after FX rows moved out
      (Keys 19 / Synth 21 / Tape 21 rows). Confirm CV Matrix, Save/Load Patch,
      Load Wave (Synth), and Tape's edit actions still fire correctly (indices
      were renumbered).
- [ ] **Web file manager waveform thumbnails** — hard-refresh the page, open
      Files, scroll: the new "Wave" column canvases populate with correct
      waveforms, lazy-load as rows enter view, survive sort/filter.
- [ ] **Web MIDI two-row piano** — black keys offset above whites, natural E–F /
      B–C gaps, labels correct, click/keys still play.
- [ ] **Drums menu** — master Delay rows still present + working (Drums did NOT
      get the FX submenu or the new effects — per-pad redesign is deferred).

## 🧪 Functional / persistence

- [ ] **"Drums lost settings between flashes"** — set Drums settings, OTA-flash,
      confirm they now survive (OTA-flush-before-reboot fix). Also try a knob-set
      value then flash.
- [ ] **`tools/pull_card.sh`** — run a full backup; confirm files land with the
      right extension (.wav/.aif/.raw) + .JSN sidecars, and a re-run skips them.

## 📋 Standing ear-queue (pre-session, from the 07-16/17 handoffs)

- [ ] Keys engine + patches, menu feel
- [ ] Broadcast out+in (:8000/live.mp3, /in.mp3)
- [ ] Tape record + FX-print pass
- [ ] Tracker module play
- [ ] Radio 48k station
