#!/usr/bin/env python3
"""What does each reverb mode actually cost, and has the NaN guard ever fired?

  tools/bench/reverb_cost.py [seconds_per_mode]

Two questions, in order of importance:

1. HAS tank_nan_guard EVER FIRED? Until 2026-07-29 a firing was an unchunked
   ~150 KB PSRAM memset in the AUDIO task — 10-15 ms of held bus against an
   8.7 ms I2S DMA buffer, i.e. a guaranteed dropout, firing unpredictably. That
   is the leading candidate for Arlo's "occasional pop from reverb". The count is
   CUMULATIVE and never cleared, so a non-zero reading is the answer. (The guard
   now flushes incrementally instead, so a fresh firing should cost a ~28 ms gap
   in the tail rather than a pop — but knowing whether it fires AT ALL is what
   decides where to look next.)

2. What each mode costs per block, against the 1450 us budget. Optimising the
   reverb before knowing this would be working on a hunch: the tank is ~40 delay-
   line accesses per sample across 24 concurrent streams in a 150 KB PSRAM slab,
   so the cost is memory traffic, and memory traffic is exactly the thing you
   cannot estimate by reading code.

`rv.us` is the reverb's own EMA of microseconds per block, reported unconditionally
(one store per block). `aus`/`auspk` are the WHOLE audio tick for comparison —
note they are structurally blind to DMA underruns, so they bound cost, not glitches.
"""
import json
import sys
import time

from rig import Rig, get, post

MODES = [(0, "off"), (1, "room"), (2, "hall"), (3, "plate"), (4, "shimmer")]
BUDGET_US = 1450.0
NOTE = 60
DEFAULT_S = 8.0

# a sustained note keeps the tank fed; dry FX slots so only the reverb is in play
PATCH = {"lm": 1, "lvl": 0.8, "base": NOTE, "quant": True,
         "atk": 0.005, "dec": 0.05, "sus": 1.0, "rel": 0.4,
         "e2c": 0.0, "fxsl": [0, 0], "rvmx": 40}


def fx_status():
    return json.loads(get("/status?fx=1"))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_S
    rig = Rig()
    st = rig.status()
    if st.get("recording"):
        raise SystemExit("ABORT: the module is RECORDING")
    if st.get("machine") != "Keys":
        print("switching to Keys (was %s)" % st.get("machine"))
        post("/remote/machine?name=Keys")
        time.sleep(4)

    s0 = fx_status()
    nan0 = (s0.get("rv") or {}).get("nan")
    print("=" * 62)
    print("NaN-guard firings since boot: %s" % nan0)
    if nan0:
        print("  ^ NON-ZERO. The tank HAS been poisoned and flushed. On firmware")
        print("    before 2026-07-29 each of these was an audible dropout.")
    else:
        print("  ^ zero so far. If the pop persists, it is NOT the NaN flush —")
        print("    look at un-slewed gd/gw on a moving mix, or elsewhere in the chain.")
    print("=" * 62)

    rig.notes_off()
    rig.set_params(PATCH, verify=False)
    time.sleep(2.0)

    print("\n mode      rv us/block   %% of 1450   aus    auspk    wet pk")
    rows = []
    for mode, name in MODES:
        post("/remote/params", json.dumps({"rv": mode}))
        time.sleep(2.5)                      # mode change fades + clears the tank
        rig.note_on(NOTE)
        samples = []
        t0 = time.time()
        while time.time() - t0 < seconds:
            rig.note_on(NOTE)                # refresh the MIDI liveness stamp
            time.sleep(1.0)
            s = fx_status()
            rv = s.get("rv") or {}
            samples.append((rv.get("us") or 0, s.get("aus") or 0,
                            s.get("auspk") or 0, rv.get("wpk") or 0))
        rig.notes_off()
        time.sleep(0.5)
        if not samples:
            continue
        # median of the settled samples; the first is taken while the tail builds
        body = samples[1:] or samples
        us = sorted(x[0] for x in body)[len(body) // 2]
        aus = sorted(x[1] for x in body)[len(body) // 2]
        pk = max(x[2] for x in body)
        wpk = max(x[3] for x in body)
        rows.append((name, us, aus, pk, wpk))
        print("  %-8s %7d      %5.1f%%   %5d  %5d    %3d%%"
              % (name, us, 100.0 * us / BUDGET_US, aus, pk, wpk))

    post("/remote/params", json.dumps({"rv": 0}))
    s1 = fx_status()
    nan1 = (s1.get("rv") or {}).get("nan")
    print("\nNaN-guard firings after the run: %s (was %s)" % (nan1, nan0))

    if rows:
        off = next((r[1] for r in rows if r[0] == "off"), 0)
        print("\n--- verdict ---")
        for name, us, aus, pk, wpk in rows:
            if name == "off":
                continue
            net = us - off
            print("   %-8s costs %4d us net of OFF = %.1f%% of the audio tick"
                  % (name, net, 100.0 * net / BUDGET_US))
        worst = max(r[1] for r in rows)
        print("\n   worst mode is %.1f%% of budget. Optimising is worth it above"
              % (100.0 * worst / BUDGET_US))
        print("   ~40%%; below ~20%% the reverb is not what to spend effort on.")


if __name__ == "__main__":
    main()
