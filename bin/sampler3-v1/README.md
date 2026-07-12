# sampler3-v1

The sampler, reborn: "Sampler" is now `machine_sampler3`, a two-voice
gate-triggered sampler rebuilt on the deck/tracker architecture (one
reader task owns all SD I/O; per-voice 1s PSRAM head-cache for instant
retrigger + 4s ring; request-flag protocol; nothing blocking in the
audio callback — the crash class that killed the old sampler is
structurally impossible). Snapshot of commit `dcbd325` (tag
`sampler3-v1-20260712`), Arlo-verified on hardware ("work of beauty").
Flash with `./flash.sh [PORT]`.

## The workflow

- Gates: TR1/TR2 trigger their track; press while playing = pause.
- HOLD a gate ~1s = arm that track (mutes it, cues the line-in through
  the monitor). Sequencer-proof: only an isolated press can arm —
  two-or-more presses inside 2.5s read as a sequence and never arm.
- Armed: press = record, press = stop + auto-load + the fresh take
  LOOPS immediately; hold mid-take = abort (file discarded).
- **Clock-synced takes** (clock on CV8, 4 PPQN): capture starts ON a
  pulse (downbeat = frame 0), stops on the next whole beat — loop-ready
  lengths by construction (instrument-verified: a take measured exactly
  36 pulses at the measured clock rate). The take's JSN sidecar gets a
  tempo stamp (`bpm`/`grid`/`dver:2`/`conf:1.0`) so recordings arrive
  pre-analyzed for the Deck; re-entry after save/load lands ON a pulse
  at the in-phase offset (`elapsed % length`).
- CV6/CV7 = per-voice through-zero speed: center unity (sticky, "1:1"
  badge), CW to +150%, CCW down through stop into reverse (max -100%).
  1V/oct (ch1/2) is a separate multiply toggle. Reverse + trim windows
  re-read via the reader (playback-order frame space).

## UI

Side-by-side voice panels mirroring the jacks: waveform thumbnail
(flips in reverse mode), thick state-colored playbar (blue idle / green
play / yellow armed / red rec) with position marker, big track numerals,
white border = encoder focus, external clock tempo BIG in the corner,
explicit ARM/REC/SYNC banner. Setup: mode/reverse/1V-oct/CV67/level/
pan/start/length + Record page (arm targets, monitor, arm-mutes).

## Engine bugs found & fixed on the way (instrument-verified)

- Ring-full deadlock: fill throttle == read margin froze the cursor
  ~3.9s into any streamed sample (starve 0/0 after fix).
- Ghost sync edges from AC-coupled pulse tails (+83.6ms takes) —
  edges now gated at 3/4 of the locked period.
- Autosave version gate (s3v) — refuses foreign blobs; the sampler2
  rename-poisoning (volume 0 on both voices) can't recur.

Sampler2 remains in the registry as a hidden fallback ("Sampler2",
reachable via the web Remote tab); removal is a follow-up.
