"""Audio gate for the display SPI clock (settings.tftclk) — 2026-09-09.

Result on .85 (Synth + reverb, held note, shadow FB allocated): control 0 events;
26 MHz 1, 40 MHz 2, 80 MHz 1 event in 30 s, all 1.6-1.9x the material ceiling,
mid-dominant — the repaint LOAD costs a marginal event now and then at every
clock; the clock itself does not add any. auspk stayed <= control.

Audio gate for the display clock: does hammering full-page repaints at a
given SPI clock put discontinuities into the analog output?  Synth + reverb,
one held MIDI note, /remote/event?ev=enter posted back-to-back (the UI queue
fills, so the repaint is continuous).  Captured through the Scarlett and run
through the bench burst detector.  Control = same note, no repaints."""
import os, sys, time, threading, subprocess, io, contextlib, json
sys.path.insert(0, "tools/bench")
os.environ.setdefault("STRAEMPLER_IP", "192.168.3.85")   # the unit with SJ1 bridged
from rig import Rig, post, get
import detect

U = "http://" + os.environ["STRAEMPLER_IP"]
SP = os.environ.get("OUT_DIR", os.path.dirname(os.path.abspath(__file__)))
SECS = 30
rig = Rig(); rig.check_device()

def settings(clk):
    subprocess.run(["curl", "-s", "-m", "5", "-X", "POST", "-H", "Content-Type: application/json",
                    "-d", '{"tftclk":%d}' % clk, U + "/settings"], stdout=subprocess.DEVNULL)

post("/remote/machine?name=Synth"); time.sleep(2.5)
rig.set_params({"rv": 1, "rvmx": 60})
get("/sysinfo?tftclear=1")

def run(label, clk, repaint):
    settings(clk); time.sleep(0.5)
    get("/sysinfo?tftclear=1")
    path = os.path.join(SP, "gate_%s.wav" % label)
    proc = rig.capture(SECS, path)
    time.sleep(1.5)
    stop = threading.Event(); n = [0]
    def hammer():
        while not stop.is_set():
            try: post("/remote/event?ev=enter", timeout=5)
            except Exception: pass
            n[0] += 1
    t = threading.Thread(target=hammer) if repaint else None
    if t: t.start()
    rig.hold_note(SECS - 4, note=57)
    rig.notes_off()
    stop.set()
    if t: t.join()
    proc.wait(timeout=SECS + 15)
    st = rig.status()
    si = json.loads(get("/sysinfo"))
    tft = si.get("tft", {})
    time.sleep(3)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        detect.report(path)
    rep = buf.getvalue().strip().splitlines()
    nev = 0 if any("no events" in l for l in rep) else max(0, len(rep) - 2)
    print("== %-8s clk=%s repaints=%d  aus=%s auspk=%s  tft.ev=%s worst=%s avg=%s n=%s  -> events=%d"
          % (label, tft.get("clk"), n[0], st.get("aus"), st.get("auspk"),
             tft.get("ev"), tft.get("worst"), tft.get("avg"), tft.get("n"), nev), flush=True)
    for l in rep: print("   " + l)

run("control", 40, False)
run("clk26", 26, True)
run("clk40", 40, True)
run("clk80", 80, True)
settings(40)
print("left at 40")
