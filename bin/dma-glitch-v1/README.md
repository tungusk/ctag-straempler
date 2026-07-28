# dma-glitch-v1 (2026-07-27)

Full firmware snapshot at `dma-glitch-v1-20260727`. Continues
`tape-fxchain-v1` (2026-07-26).

Serial-flash (recovery only — normal updates are `tools/ota.sh <IP>`):

    ./flash.sh [/dev/cu.usbserial-XXXX]

OTA layout (app @ 0x20000). Flashing an OLD single-app archive (e.g.
`bin/synth-v1`) reverts OTA and needs a re-migration.

## The headline: HTTP requests were glitching the audio

Chased for days as "an occasional short burst of noise", finally caught with a
new analog capture rig rather than by ear or by on-device meters.

**~28% of HTTP requests to the module produced an audible discontinuity.** With
the web UI polling `/status` twice a second, that is a glitch every ~1.6 s.

    condition                        before      after
    gate refresh only (0.5 req/s)    0.089/s     0.044/s
    + GET /status at 2 Hz            0.644/s     0.078/s    88% fewer

Narrowed by elimination, each step measured:

- Event rate scales with REQUEST rate, not with anything musical — 3x the posts
  gave 3.8x the events, ~0.28 events per request on both the trigger path and
  plain GETs.
- A ping flood does NOT do it (1.1x), so it is not the radio or the network
  stack. 1400-byte pings at the same rate as the poll do not either (0.7x), so
  it is not the size of the WiFi TX burst. That leaves httpd's request handling.
- In the waveform it is a PHASE DISCONTINUITY: the signal jumps and carries on
  normally from the new position — a repeated or dropped DMA buffer.

**Cause:** `i2s_per.c` had `dma_buf_count = 4, dma_buf_len = 32` — 128 frames =
**2.9 ms** of total slack, less than one HTTP request needs. Now **4 x 96 =
8.7 ms**, costing ~6 ms of output latency.

**Get the depth from `dma_buf_len`, not `dma_buf_count`.** `12 x 32` is the same
8.7 ms and makes KEYS AUDIBLY SCRATCHY (its slew ceiling goes 0.027 -> 0.142 on a
sustained note); `4 x 96`, `4 x 192` and `8 x 32` are all clean. The harm is the
descriptor count. **Validate I2S changes on Keys** — it streams from PSRAM and is
the host that shows this. `tools/bench/sweep_dma.py` does both measurements per
configuration in one command.

**Why the meters missed it, which is the transferable lesson:** `i2s_write` uses
`portMAX_DELAY`, so after an underrun the DMA is empty, the write returns
immediately and the loop catches up. `aus`, `auspk` and the new `ausgap` all
looked innocent through every glitch. A meter that times the TASK cannot see the
HARDWARE running dry.

## Also in this build

**Keys could become permanently unloadable** (fixed `f2055c5`). `keys_load_zone`
asks for ONE 2,000,000-byte SPIRAM block; `keys_stop` frees it (correctly — Tape
wants up to 3.62 MB of the 4 MB pool), but re-acquiring it depends on PSRAM not
having fragmented. Measured largest free block **1,998,848 — 1,152 bytes short**,
after which every load silently returned -1 for the rest of the session and only
a power cycle recovered it. Trigger is ordinary use: leave Keys, enable an FX
slot, come back. Now an allocation ladder (2.0 -> 1.5 -> 1.0 -> 0.5 -> 0.24 MB)
with the achieved size in `zone.cap`, so a shorter maximum sample loads instead
of nothing at all. A failed load now says so on screen instead of no-opping, and
an empty sample slot draws a cursor (it drew none, so the one element you must
reach to fix an empty Keys could not show it was selected).

**`slot_gc` now runs on the preset path.** It had only ever been wired to the
menu, so loading a patch whose slots are Off left the delay's ~690 KB and the
flanger's ~90 KB allocated indefinitely — measured ZERO reclaimed. This is the
other half of the 07-26 "a delay auditioned once hurt the FX quality" find:
**+709,840 bytes** reclaimed.

**Overdrive Bias no longer mutes the stage.** Bias entered as
`tanh(g*(x+bias))` with `g = 1+29*drive`, so at drive 80% a bias of 0.8 landed at
`tanh(19)` — the shaper pinned at +1 and the DC subtraction left zero. Measured
silent output at bias >= 40% for every drive setting; over half the knob was a
dead zone. Bias is now an offset inside the shaper, independent of `g`.

**New meters** (`/status`): `ausgap` peak block-to-block interval, `sav`
autosave duration + count (a save costs ~64 ms), and per-stage FX meters behind
`?fx=1` — armed on demand, because measuring is per-sample work in the shared
audio path and leaving it on was itself audible.

**`tools/proof_build.sh` passes again** — it had been failing since `5b52510`
(undefined `fxrack_peak_pct`: with every machine excluded nothing pulls fxrack
into the link, and `/status` is core).

## Ear debt

- Overdrive **Bias** sweep by ear — the curve changed.
- OD level on **Drums** (the 6 dB trim was global; Synth and Keys are confirmed).
- Tape `post` vs `pre` printing with Drive up.
- Empty-slot cursor and load-failure message, by eye.

## Known open

**State-dependent scratchiness with flanger + reverb on Keys.** Mid-session it
measured a slew ceiling of 0.1928 with 1.50 events/s where clean is ~0.028 —
Arlo heard it at once. Either effect alone was clean. `POST /reboot` cleared it
completely, same firmware and patch either side. Could NOT be reproduced on
demand afterwards (cycling FX slabs did not do it; three machine-switch cycles
moved PSRAM's largest block but left the audio clean). Same family as the 07-26
wandering-pitch bug. **If it recurs, capture BEFORE rebooting** — the reboot is
the cure and destroys the evidence.

Arlo's verdict on this build: "smaller, isolated pops when flanger is on, may be
tolerable now."
