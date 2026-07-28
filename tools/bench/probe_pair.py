#!/usr/bin/env python3
"""WHY does flanger + reverb glitch when neither does alone?

Established first (compare_fx.py, interleaved, 24 blocks): dry / hall / flanger
each produce 0.00 events per second of sound, flanger+hall produces 0.44-0.77.
The harness floor is zero, so anything here is the module.

The discriminators, cheapest first:

  flg_mix0_hall   flanger in the slot but mixed OUT. If it still glitches, the
                  flanger's SIGNAL is irrelevant and the cause is its presence —
                  a second PSRAM slab, or the extra stage in the chain.
  dly_hall        DELAY + reverb. The delay also allocates a PSRAM slab (~690 KB
                  vs the flanger's ~90 KB). If this glitches too, the fault is
                  "two slabs / two stages", not the flanger specifically.
  trem_hall       TREMOLO + reverb. Tremolo allocates NOTHING. If this is clean
                  while delay+reverb is not, the slab is implicated rather than
                  the stage count.
  flg_room/plate  does the reverb MODE matter (different tank sizes)?
  flg_fb0_hall    flanger with zero feedback — no recirculation.
"""
import sys
import threading
import time

import numpy as np

from detect import find_bursts
from rig import Rig, load_wav

SEG = 24.0
NOTE_EVERY = 3.0
ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 3

# fxsl kinds: 0 Off 1 OD 2 Flanger 3 Trem 4 Delay ; rv: 0 off 1 room 2 hall 3 plate
CASES = [
    ("dry",            {"fxsl": [0, 0], "rv": 0}),
    ("flg_hall",       {"fxsl": [2, 0], "rv": 2}),
    ("flg_mix0_hall",  {"fxsl": [2, 0], "rv": 2, "flgmx": 0}),
    ("flg_fb0_hall",   {"fxsl": [2, 0], "rv": 2, "flgfb": 0}),
    ("dly_hall",       {"fxsl": [4, 0], "rv": 2}),
    ("trem_hall",      {"fxsl": [3, 0], "rv": 2}),
    ("flg_room",       {"fxsl": [2, 0], "rv": 1}),
    ("flg_plate",      {"fxsl": [2, 0], "rv": 3}),
]

rig = Rig()
base = rig.params()
acc = {n: {"ev": 0, "snd": 0.0} for n, _ in CASES}
print("patch: smp %r rvmx %s lvl %.2f\n%d rounds, %.0fs blocks, interleaved\n"
      % (base.get("smp"), base.get("rvmx"), base.get("lvl", 0), ROUNDS, SEG))
try:
    for rnd in range(ROUNDS):
        for name, over in CASES:
            # two-step: slot first so the lazy slab exists, then its params
            rig.set_params(dict(base, fxsl=over["fxsl"], rv=over["rv"]), verify=False)
            time.sleep(1.0)
            extra = {k: v for k, v in over.items() if k not in ("fxsl", "rv")}
            if extra:
                rig.set_params(dict(base, **over), verify=False)
                time.sleep(0.8)
            stop = threading.Event()

            def notes():
                while not stop.is_set():
                    try:
                        rig.trigger(1500)
                    except Exception:                 # noqa: BLE001
                        pass
                    stop.wait(NOTE_EVERY)
            th = threading.Thread(target=notes, daemon=True)
            th.start()
            wav = "pair_%s.wav" % name
            try:
                rig.capture_blocking(SEG, wav)
            except Exception as exc:                  # noqa: BLE001
                stop.set(); th.join(timeout=3)
                print("capture failed: %s" % exc)
                raise SystemExit(1)
            stop.set(); th.join(timeout=3)
            x, sr = load_wav(wav)
            m = np.abs(x).mean(axis=1)
            k = int(0.25 * sr)
            env = np.sqrt(np.mean((m[:len(m) // k * k].reshape(-1, k)) ** 2, axis=1))
            snd = 0.25 * float(np.sum(env > 0.01))
            ev, thr, ceil = find_bursts(x, sr)
            acc[name]["ev"] += len(ev)
            acc[name]["snd"] += snd
            print("  r%d %-15s %2d events / %4.1f s sound = %.2f/s"
                  % (rnd + 1, name, len(ev), snd, len(ev) / max(snd, 0.1)), flush=True)
finally:
    rig.set_params(base, verify=False)

print("\n=== events per second OF SOUND ===")
for n, _ in CASES:
    d = acc[n]
    print("  %-15s %.2f/s   (%d events, %.1f s)"
          % (n, d["ev"] / max(d["snd"], 0.1), d["ev"], d["snd"]))
