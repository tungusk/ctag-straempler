# flanger-fix-v1 (2026-07-27)

Full firmware snapshot at `flanger-fix-v1-20260727`. **Supersedes
`dma-glitch-v1` from the same day** — that archive predates the flanger fix and
the `/remote/trig` change. Use this one.

Serial-flash (recovery only — normal updates are `tools/ota.sh <IP>`):

    ./flash.sh [/dev/cu.usbserial-XXXX]

OTA layout (app @ 0x20000).

## Two audible faults found and fixed, both measured on the analog output

Neither was visible to any meter on the device. Both were found with the new
`tools/bench` capture rig, and both are DMA underruns — the output jumps and
carries on from the new position, the signature of a dropped buffer.

### 1. HTTP requests glitched the audio

**~28% of requests produced an audible discontinuity.** With the web UI polling
`/status` twice a second that is a glitch every ~1.6 s.

    condition                       before     after
    gate refresh only (0.5 req/s)   0.089/s    0.044/s
    + GET /status at 2 Hz           0.644/s    0.078/s

A ping flood does NOT do it, and neither do 1400-byte pings at the same rate, so
it is not the radio and not the size of the TX burst — it is httpd's request
handling. The I2S DMA had `4 x 32 = 128 frames = 2.9 ms` of slack, less than one
request needs. Now `4 x 96 = 8.7 ms`.

**Get the depth from `dma_buf_len`, not `dma_buf_count`.** `12 x 32` is the same
8.7 ms and makes Keys audibly SCRATCHY (its slew ceiling goes 0.027 -> 0.142 on a
sustained note). `4 x 96`, `4 x 192` and `8 x 32` are all clean.
**Validate I2S changes ON KEYS** — it streams from PSRAM and is the host that
shows this. `tools/bench/sweep_dma.py` does it in one command.

### 2. Flanger + reverb glitched, and neither alone did

Interleaved, with the harness floor measured at ZERO in the same session:

    dry              0.000/s     (0 events in 259 s — same notes, same posts)
    flanger alone    0.000/s
    hall alone       0.009/s
    flanger + hall   0.805/s     (267 events in 332 s)

It was never the signal — it survived mixing the flanger OUT (0.82/s), zeroing
its feedback (1.14/s) and zeroing its sweep depth (0.87/s). It was not "two
stages" or "a second PSRAM slab" either: DELAY + hall measured 0.07/s with a
BIGGER slab, and TREMOLO + hall 0.04/s. It scaled with tank size — room 0.10,
plate 0.30, hall 1.33.

The flanger called `cosf()` 64 times per block for an LFO running at 0.01-10 Hz,
which moves ~1.5% of a cycle across one 1.45 ms block. Computing it at both ends
and interpolating removes the cost; the flanger still modulates 81% envelope
depth on a sustained note. Confirmed two independent ways:

    8.7 ms DMA,  cosf per sample   1.33/s
    17.4 ms DMA, cosf per sample   0.122/s   (buffer absorbs it)
    8.7 ms DMA,  per-block LFO     0.103/s   <- shipped, keeps the latency

Arlo's verdict: "sounds better."

## THE LESSON WORTH CARRYING

**No meter on the device can see an underrun.** flanger+hall reads `aus` 706 /
`auspk` 1256 / `ausgap` 1349; delay+hall reads 646/1238/1339 and is 15x cleaner.
`i2s_write` uses `portMAX_DELAY`, so after the hardware runs dry the write
returns immediately and the loop catches up — the timing looks perfect while
audio is being lost. A meter that times the TASK cannot see the HARDWARE running
dry. Only analog capture sees it.

## Also in this build

- **Keys could become permanently unloadable** — one 2,000,000-byte SPIRAM block,
  freed on machine switch, unobtainable once PSRAM fragmented (measured largest
  free block 1,998,848, **1,152 bytes short**). Now an allocation ladder with the
  achieved size in `zone.cap`; failed loads say so on screen; an empty sample slot
  draws a cursor.
- **`slot_gc` now runs on the preset path** — loading a patch with slots Off used
  to reclaim nothing. **+709,840 bytes**.
- **Overdrive Bias no longer mutes the stage** (bias was scaled by the full input
  gain, so >=40% pinned `tanh` and the DC subtraction left zero).
- **New meters**: `ausgap`, `sav`, and per-stage FX meters behind `/status?fx=1`,
  armed on demand.
- `/remote/trig` accepts up to 60 s and echoes the ms it used.
- `tools/proof_build.sh` passes.

## Ear debt

- Overdrive **Bias** sweep — the curve changed.
- OD level on **Drums** (Synth and Keys confirmed).
- Tape `post` vs `pre` printing with Drive up.
- Empty-slot cursor and load-failure message, by eye.

## Known open

- ~~Keys' sustain loop does not loop~~ — **WRONG, retracted.** The loop is fully
  built (engine wrap, seam crossfade, Setup rows, preset persistence) and works:
  with `lm=1` the output VU stays alive indefinitely, with `lm=0` it stops after
  one sample length. The original test was contaminated by the soft-gate bug
  below — the gate released at ~2 s, so the note stopped and looked like a
  failed loop.
- **Keys stops a note dead when the sample runs out** — the voice is zeroed with
  no fade once `pos >= frames`, so any sample not ending at zero clicks.
- **Long soft gates release early, intermittently** — a 30 s `/remote/trig` held
  8+ s once and 2.3 s another time, with the deadline verifiably set to 3000
  ticks. Unexplained.
