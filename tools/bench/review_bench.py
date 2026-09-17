#!/usr/bin/env python3
"""Bench checks for the 2026-09-16 code-review fixes (tiers 1-3).

  tools/bench/review_bench.py                         # REST only
  tools/bench/review_bench.py --serial /dev/cu.usbserial-XXXX --audio
  tools/bench/review_bench.py --only keys,synth --reps 10
  tools/bench/review_bench.py --only lfn --reboot

Drives the unit over REST through the checklist in plans/code-review-2026-09.md
(tier 1, "Bench checks") and judges each step WITHOUT a human:

- crash:  /sysinfo uptime going backwards (and the sticky `reset` string).
- Stub:   /status machine == "Stub" after a switch means a start() failed
          because an old task outlived its stop (machine_activate's fallback).
- leak:   with --serial, the log lines "leaking" / "still running", plus any
          panic. Opening the port REBOOTS the module, so it is opened first
          and the run waits for boot.
- noise:  with --audio (Scarlett), each test is recorded and detect.py's burst
          finder runs over it. Events near a switch or a load are listed with
          their offset, and a clip is kept for listening. The detector cannot
          tell a click from music, so these are CANDIDATES for the ear.

Panel-only actions are reached through the code they call, not the encoder:
Keys Clear Zones / Load Sample -> a `zones` preset runs keys_clear_zones +
keys_load_zone_at; Synth Load Wave -> `wave` runs synth_load_wave; Tape length
-> `lsel` runs tape_set_len_sel. Editor/Freesound AUDITION start/stop has no
web route and is NOT covered: that one stays a hand check.
"""
import argparse
import json
import math
import os
import random
import re
import signal
import socket
import struct
import sys
import threading
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "review_out")

# ---- REST --------------------------------------------------------------------
IP = None


def _req(method, path, body=None, headers=None, timeout=15):
    req = urllib.request.Request("http://%s%s" % (IP, path), data=body,
                                 method=method, headers=headers or {})
    for i in range(3):
        try:
            return urllib.request.urlopen(req, timeout=timeout).read()
        except urllib.error.HTTPError as exc:
            return exc.read() or str(exc).encode()
        except Exception as exc:                      # noqa: BLE001 - transport
            if i == 2:
                raise
            time.sleep(1.0 + i)
    return b""


def get_json(path, timeout=15):
    return json.loads(_req("GET", path, timeout=timeout))


def post(path, body=None, timeout=15):
    if isinstance(body, (dict, list)):
        body = json.dumps(body)
    return _req("POST", path, body.encode() if body else b"", timeout=timeout)


def post_async(path, body=None):
    t = threading.Thread(target=lambda: _safe(post, path, body), daemon=True)
    t.start()
    return t


def _safe(fn, *a):
    try:
        fn(*a)
    except Exception:                                 # noqa: BLE001
        pass


# ---- result log ----------------------------------------------------------------
class Log:
    def __init__(self):
        self.t0 = time.time()
        self.fails = []
        self.notes = []
        self.fh = open(os.path.join(OUT, "run.log"), "w")

    def say(self, msg):
        line = "[%6.1f] %s" % (time.time() - self.t0, msg)
        print(line, flush=True)
        self.fh.write(line + "\n")
        self.fh.flush()

    def fail(self, test, msg):
        self.fails.append((test, msg))
        self.say("FAIL %s: %s" % (test, msg))

    def note(self, test, msg):
        self.notes.append((test, msg))
        self.say("note %s: %s" % (test, msg))


LOG = None


# ---- serial ------------------------------------------------------------------
SERIAL_FLAGS = re.compile(r"leaking|still running|failed to start|Guru Meditation|"
                          r"abort\(\)|Backtrace|stack overflow|assert failed|CORRUPT|"
                          r"parse failed|won't clobber|URI swap|SHORT WRITE",
                          re.IGNORECASE)


class SerialTap:
    def __init__(self, port):
        import serial                                 # pyserial
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.fh = open(os.path.join(OUT, "serial.log"), "w", errors="replace")
        self.hits = []
        self.test = "boot"
        self.stop = False
        self.th = threading.Thread(target=self.run, daemon=True)
        self.th.start()

    def run(self):
        buf = b""
        while not self.stop:
            try:
                chunk = self.ser.read(4096)
            except Exception:                         # noqa: BLE001
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                self.fh.write("%.1f %s\n" % (time.time() - LOG.t0, line))
                if SERIAL_FLAGS.search(line):
                    self.hits.append((self.test, line))
                    LOG.say("SERIAL [%s] %s" % (self.test, line))
            self.fh.flush()

    def take(self, test):
        """Hits since the last call, and switch the label to `test`."""
        h = [x for x in self.hits]
        self.hits = []
        self.test = test
        return h


SER = None


# ---- health ------------------------------------------------------------------
class Health:
    def __init__(self):
        si = get_json("/sysinfo")
        self.uptime = si["uptime"]
        self.reset = si["reset"]
        self.wall = time.time()
        LOG.say("unit: uptime %d s, reset %s, iram big %d, %s"
                % (si["uptime"], si["reset"], si["iram"]["big"], si["version"][:24]))

    def check(self, test):
        """-> True if the unit has not rebooted since the last check."""
        try:
            si = get_json("/sysinfo", timeout=20)
        except Exception as exc:                      # noqa: BLE001
            LOG.fail(test, "unit not answering (%s) — crashed or wedged? "
                           "DON'T reboot it before capturing serial" % exc)
            return False
        ok = True
        # uptime is seconds since boot; it may lag wall time, never go backwards
        if si["uptime"] + 2 < self.uptime:
            LOG.fail(test, "REBOOTED: uptime %d -> %d, reset '%s'"
                     % (self.uptime, si["uptime"], si["reset"]))
            ok = False
        self.uptime, self.reset = si["uptime"], si["reset"]
        if si["iram"]["big"] < 16000:
            LOG.note(test, "internal RAM largest free block down to %d" % si["iram"]["big"])
        if SER:
            for t, line in SER.take(test):
                LOG.fail(t, "serial: " + line)
        return ok

    def rebased(self):
        self.uptime = 0


HEALTH = None


def machine_now(timeout=20):
    return get_json("/status", timeout=timeout).get("machine", "")


def switch(name, test, timeout=20.0):
    """Switch and wait for /status to show it. Returns the machine reached."""
    post("/remote/machine?name=%s" % urllib.parse.quote(name))
    t_end = time.time() + timeout
    got = ""
    while time.time() < t_end:
        time.sleep(0.5)
        try:
            got = machine_now()
        except Exception:                             # noqa: BLE001
            continue
        if got == name:
            return got
        if got == "Stub":
            LOG.fail(test, "switch to %s landed on STUB — an old task outlived "
                           "its stop (look for 'leaking'/'still running')" % name)
            return got
    LOG.fail(test, "switch to %s: still '%s' after %.0f s" % (name, got, timeout))
    return got


def params():
    return get_json("/remote/params")


def set_params(patch, settle=1.0):
    post("/remote/params", patch)
    time.sleep(settle)


# ---- audio -------------------------------------------------------------------
class Recorder:
    """One capture per test, stopped with SIGINT so sox finalises the header."""

    def __init__(self, enabled):
        self.rig = None
        if enabled:
            sys.path.insert(0, HERE)
            from rig import Rig                       # noqa: E402
            self.rig = Rig()
            self.rig.check_device()
        self.proc = None
        self.marks = []

    def start(self, test):
        self.marks = []
        if not self.rig:
            return
        from rig import CAPTURE_OPEN_S
        self.path = os.path.join(OUT, "%s.wav" % test)
        self.proc = self.rig.capture(3600, self.path)
        time.sleep(CAPTURE_OPEN_S)
        self.t0 = time.time()

    def mark(self, what):
        if self.rig and self.proc:
            self.marks.append((time.time() - self.t0, what))

    def stop(self, test, window=0.6):
        if not self.rig or not self.proc:
            return
        time.sleep(1.0)
        self.proc.send_signal(signal.SIGINT)
        try:
            self.proc.wait(timeout=10)
        except Exception:                             # noqa: BLE001
            self.proc.kill()
        self.proc = None
        time.sleep(0.8)
        from detect import find_bursts
        from rig import load_wav
        try:
            x, sr = load_wav(self.path)
        except Exception as exc:                      # noqa: BLE001
            LOG.note(test, "capture unreadable: %s" % exc)
            return
        ev, thr, base = find_bursts(x, sr)
        near = []
        for e in ev:
            m = [(t, w) for t, w in self.marks if 0 <= e["t"] - t <= window]
            if m:
                near.append((e, m[-1]))
        LOG.say("audio %s: %.0f s, %d events, %d within %.1f s of a switch/load"
                % (test, len(x) / sr, len(ev), len(near), window))
        for e, (t, w) in near:
            LOG.note(test, "click candidate %.3f s (+%d ms after '%s'), step %.3f x%.1f %s"
                     % (e["t"], (e["t"] - t) * 1000, w, e["max_step"], e["over_ceiling"],
                        "SIDE" if e["side_dominant"] else "mid"))
            self._clip(x, sr, e["t"], "%s_%07.3f.wav" % (test, e["t"]))

    def _clip(self, x, sr, t, name, pad=0.5):
        import numpy as np
        import wave
        a, b = max(0, int((t - pad) * sr)), min(len(x), int((t + pad) * sr))
        seg = (np.clip(x[a:b], -1, 1) * 32767).astype("<i2")
        with wave.open(os.path.join(OUT, name), "wb") as w:
            w.setnchannels(seg.shape[1])
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes(seg.tobytes())


REC = None


# ---- material ------------------------------------------------------------------
def pick_material():
    files = get_json("/files", timeout=60)["files"]
    by = {}
    for f in files:
        by.setdefault(f["dir"], []).append(f)
    pool = sorted(by.get("pool", []), key=lambda f: -f["size"])
    small = [f["name"] for f in files if 20000 < f["size"] < 600000]
    keys = [f["name"] for f in by.get("KEYS", []) if f["size"] > 20000]
    mods = sorted(get_json("/trk/list")["modules"], key=lambda m: -m["size"])
    mat = {
        "big": [f["name"] for f in pool[:2]] or small[:2],
        "keys": (keys + small)[:2],
        "wave": small[:2],
        "module": mods[0]["name"] if mods else None,
        # long pool tracks with no tempo stamp: loading one starts a BPM analysis
        "unan": [f["name"] for f in pool if not f.get("bpm") and f["size"] > 5e6][:8],
    }
    LOG.say("material: %s" % mat)
    return mat


# ---- tests -------------------------------------------------------------------
def settle_rand(lo, hi):
    d = random.uniform(lo, hi)
    time.sleep(d)
    return d


def t_switch(mat, reps):
    """1. Switch away while each machine is busy loading, playing or saving."""
    test = "switch"
    REC.start(test)
    big0, big1 = (mat["big"] + mat["big"])[:2]
    exits = ["Synth", "Keys", "Freesound", "Tape"]    # varied starts on the way out

    def sampler():
        set_params({"voices": [{"name": big0}, {"name": big1}]}, settle=0.3)
        post("/remote/trig?t=1&ms=3000"); post("/remote/trig?t=2&ms=3000")

    def deck():
        set_params({"track": big0, "auto_an": True}, settle=0.2)
        post("/remote/trig?t=1&ms=60")

    def dualdeck():
        set_params({"ta": big0, "tb": big1}, settle=0.2)
        post("/remote/trig?t=1&ms=60")

    def tracker():
        if mat["module"]:
            post("/remote/params", {"file": mat["module"]})

    def radio():
        post("/radio/play?station=0")

    def looper():
        post_async("/looper/save?trk=1")

    def granular():
        post("/remote/params", {"sample": big1})

    def slicer():
        post("/remote/trig?t=1&ms=500")

    # (machine, busy action, delay range before switching away)
    scenarios = [("Sampler", sampler, (0.2, 2.5)),
                 ("Deck", deck, (0.1, 3.0)),
                 ("Tracker", tracker, (0.05, 1.5)),
                 ("Radio", radio, (0.2, 4.0)),
                 ("Looper", looper, (0.05, 0.8)),
                 ("Granular", granular, (0.05, 1.5)),
                 ("Slicer", slicer, (0.1, 1.5)),
                 ("Editor", lambda: None, (0.3, 1.0)),
                 ("Freesound", lambda: None, (0.3, 1.0))]
    scenarios.append(("DoubleDecker", dualdeck, (0.1, 3.0)))

    k = 0
    for r in range(reps):
        for name, busy, (lo, hi) in scenarios:
            label = "%s#%d" % (name, r + 1)
            if switch(name, test) != name:
                HEALTH.check(label)
                continue
            try:
                busy()
            except Exception as exc:                  # noqa: BLE001
                LOG.note(test, "%s busy action: %s" % (label, exc))
            d = settle_rand(lo, hi)
            nxt = exits[k % len(exits)]
            k += 1
            REC.mark("%s->%s" % (name, nxt))
            got = switch(nxt, test)
            LOG.say("switch %s busy %.2f s -> %s: %s" % (label, d, nxt,
                                                        "ok" if got == nxt else got))
            HEALTH.check(label)
    post("/remote/trig?t=1&ms=5"); post("/remote/trig?t=2&ms=5")
    REC.stop(test)


def hold(note, until):
    """Keep a MIDI note alive (5 s liveness) while sleeping until `until`."""
    while time.time() < until:
        post("/midi/on?note=%d" % note)
        time.sleep(min(1.4, max(0.0, until - time.time())))


def t_keys(mat, reps):
    """2. Keys: Clear Zones + Load Sample with a note held."""
    test = "keys"
    if switch("Keys", test) != "Keys":
        return
    before = params().get("zones")
    a, b = (mat["keys"] + mat["keys"])[:2]
    REC.start(test)
    post("/midi/on?note=48")
    for i in range(reps):
        name = a if i % 2 == 0 else b
        REC.mark("zones=%s" % name)
        post("/remote/params", {"zones": [{"smp": name, "root": 48}]})
        hold(48, time.time() + 1.5)
        HEALTH.check("keys#%d" % (i + 1))
    post("/midi/alloff")
    back = params().get("zones") or []
    if not back or back[0].get("smp") != (a if (reps - 1) % 2 == 0 else b):
        LOG.fail(test, "zone readback after loads: %s" % back[:1])
    REC.stop(test)
    if before:
        set_params({"zones": before})


def t_tape(mat, reps):
    """3. Tape: change the length, then TR1 and TR2.

    tape_set_len_sel refuses unless the transport is STOPPED, and REST cannot
    see the transport. Leaving and re-entering Tape is the one reliable stop,
    so each rep starts with that. TR2 punches a take (usr/TAPE fills a little).
    """
    test = "tape"
    if switch("Tape", test) != "Tape":
        return
    orig = params().get("lsel", 1)
    tape_before = tape_files()
    REC.start(test)
    for i in range(reps):
        if i and (switch("Synth", test) != "Synth" or switch("Tape", test) != "Tape"):
            break
        sel = (orig + i + 1) % 3
        REC.mark("lsel=%d" % sel)
        post("/remote/params", {"lsel": sel})
        time.sleep(random.uniform(0.0, 0.3))            # sometimes before the realloc lands
        # TR2 from a STOPPED tape is a fresh take; TR1 first would start playing
        # an empty tape and make the punch an overdub of nothing (no take, no
        # save). 200 ms pulses: 60 ms ones did not register on Tape (bench 09-16)
        REC.mark("TR2 in")
        post("/remote/trig?t=2&ms=200")
        time.sleep(1.5)
        REC.mark("TR2 out")
        post("/remote/trig?t=2&ms=200")
        time.sleep(1.5)
        REC.mark("TR1")
        post("/remote/trig?t=1&ms=200")          # play/stop over the take just made
        time.sleep(0.8)
        got = params().get("lsel")
        if got != sel:
            LOG.fail(test, "lsel readback %s, wanted %d" % (got, sel))
        HEALTH.check("tape#%d" % (i + 1))
    REC.stop(test)
    if switch("Synth", test) == "Synth" and switch("Tape", test) == "Tape":
        set_params({"lsel": orig})
    # 11.5: each punched take is saved ONCE; leaving Tape used to save it again
    time.sleep(2)
    new = tape_files() - tape_before
    LOG.say("tape: %d new take files for %d punch-outs" % (len(new), reps))
    if len(new) > reps:
        LOG.fail(test, "%d take files for %d takes — duplicate saves: %s" % (len(new), reps, sorted(new)))


def t_synth(mat, reps):
    """4. Synth: load a wave with a note held — silence during the load, not noise."""
    test = "synth"
    if switch("Synth", test) != "Synth":
        return
    p0 = params()
    a, b = (mat["wave"] + mat["wave"])[:2]
    REC.start(test)
    post("/midi/on?note=48")
    for i in range(reps):
        name = a if i % 2 == 0 else b
        REC.mark("wave=%s" % name)
        post("/remote/params", {"wave": name})
        hold(48, time.time() + 1.5)
        HEALTH.check("synth#%d" % (i + 1))
    post("/midi/alloff")
    if params().get("wave") not in (a, b):
        LOG.fail(test, "wave readback '%s'" % params().get("wave"))
    REC.stop(test)
    set_params({"eng": p0.get("eng", 1)})



def tape_files():
    return {f["name"] for f in get_json("/files", timeout=60)["files"] if f["dir"] == "TAPE"}


def t_decks(mat, reps):
    """Theme D: leave Deck / DoubleDecker while a BPM analysis runs.

    The analysis now stops with the machine, so the track must stay UNSTAMPED
    (an aborted run writes nothing) and nothing may outlive the switch."""
    test = "decks"
    un = list(mat["unan"])
    if len(un) < 3:
        LOG.note(test, "fewer than 3 unanalysed long tracks; skipped")
        return
    for r in range(reps):
        a, b, c = un[r % len(un)], un[(r + 1) % len(un)], un[(r + 2) % len(un)]
        if switch("Deck", test) == "Deck":
            set_params({"track": a, "auto_an": True}, settle=0.2)
            d = settle_rand(0.5, 4.0)
            got = switch(["Synth", "DoubleDecker", "Keys"][r % 3], test)
            LOG.say("decks: Deck analysing %s, left after %.1f s -> %s" % (a, d, got))
            HEALTH.check("decks-deck#%d" % (r + 1))
        if switch("DoubleDecker", test) == "DoubleDecker":
            set_params({"ta": b, "tb": c}, settle=0.3)
            time.sleep(random.uniform(0.2, 1.5))
            set_params({"ta": a}, settle=0.2)       # re-load deck A mid-analysis (10.5)
            d = settle_rand(0.3, 3.0)
            got = switch(["Deck", "Synth", "Freesound"][r % 3], test)
            LOG.say("decks: DoubleDecker analysing, left after %.1f s -> %s" % (d, got))
            HEALTH.check("decks-dd#%d" % (r + 1))
    time.sleep(5)                                    # a leaked run would finish and stamp now
    stamped = {f["name"]: f.get("bpm") for f in get_json("/files", timeout=60)["files"]}
    wrong = [n for n in un[:reps + 2] if stamped.get(n)]
    if wrong:
        LOG.note(test, "tracks stamped although every run was interrupted: %s" % wrong)


def t_stall(mat, reps):
    """6.6: a client that sends half a body and goes quiet must not hang httpd."""
    test = "stall"
    sk = socket.create_connection((IP, 80), timeout=5)
    sk.sendall(b"POST /remote/params HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
               b"Content-Length: 400\r\n\r\n{\"lvl\":")
    t0 = time.time()
    time.sleep(1.0)
    ok = False
    while time.time() - t0 < 60:
        try:
            get_json("/status", timeout=8)
            ok = True
            break
        except Exception:                             # noqa: BLE001
            time.sleep(1)
    dt = time.time() - t0
    sk.close()
    if ok:
        LOG.say("stall: /status answered %.1f s after a stalled body" % dt)
    else:
        LOG.fail(test, "httpd still hung 60 s after a stalled request body")
    HEALTH.check(test)


def t_uri(mat, reps):
    """6.11: switch between machines with web URIs while those URIs are polled."""
    test = "uri"
    stop = [False]
    hits = [0]

    def poll():
        paths = ["/status", "/fs/state", "/radio/state", "/edit/state", "/looper/save?trk=9"]
        i = 0
        while not stop[0]:
            try:
                _req("GET", paths[i % 4])
                hits[0] += 1
            except Exception:                         # noqa: BLE001
                pass
            i += 1
            time.sleep(0.1)
    th = threading.Thread(target=poll, daemon=True)
    th.start()
    ring = ["Freesound", "Radio", "Editor", "Synth", "Looper"]
    for i in range(reps * len(ring)):
        switch(ring[i % len(ring)], test)
        HEALTH.check("uri#%d" % (i + 1))
    stop[0] = True
    th.join(timeout=5)
    LOG.say("uri: %d switches under %d polled requests" % (reps * len(ring), hits[0]))


def _wav_bytes(fmt_chunk, frames_bytes, extra_chunks=b""):
    data = b"data" + struct.pack("<I", len(frames_bytes)) + frames_bytes
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk + extra_chunks + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def _upload(name, blob):
    r = _req("PUT", "/drop_sample", blob, {"Name": name}, timeout=60)
    t0 = time.time()
    time.sleep(1.5)
    while time.time() - t0 < 60:                      # the upload kicks the importer
        st = get_json("/import", timeout=20)
        if not st.get("busy"):
            break
        time.sleep(1)
    return r


def t_import(mat, reps):
    """3.5 ID3 mp3, 3.10 extensible 24-bit WAV, 3.6 corrupt chunk size."""
    test = "import"
    import subprocess
    import wave
    sr = 44100
    tone = b"".join(struct.pack("<hh", s, s) for s in
                    (int(9000 * math.sin(2 * math.pi * 330 * i / sr)) for i in range(sr * 2)))
    names = {f["name"]: f for f in get_json("/files", timeout=60)["files"]}
    for n in ("REVIEWID3", "REVIEWEXT", "REVIEWBAD"):
        if n in names:
            _req("DELETE", "/files?name=%s" % n)
    # 3.5: an mp3 with a 16 KB ID3v2 tag in front
    wav = os.path.join(OUT, "id3src.wav")
    with wave.open(wav, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(tone)
    mp3 = os.path.join(OUT, "id3.mp3")
    subprocess.run(["lame", "--quiet", "--add-id3v2", "--pad-id3v2-size", "16384",
                    "--tt", "review", wav, mp3], check=True)
    _upload("REVIEWID3", open(mp3, "rb").read())
    # 3.10: WAVE_FORMAT_EXTENSIBLE, 24-bit 48 kHz
    sr2 = 48000
    pcm24 = b"".join(struct.pack("<i", int(2000000 * math.sin(2 * math.pi * 330 * i / sr2)))[:3] * 2
                     for i in range(sr2 * 2))
    guid = bytes.fromhex("0100000000001000800000aa00389b71")
    fmt = struct.pack("<HHIIHHHHI", 0xFFFE, 2, sr2, sr2 * 6, 6, 24, 22, 24, 3) + guid
    _upload("REVIEWEXT", _wav_bytes(fmt, pcm24))
    # 3.6: a chunk whose size wraps 32-bit position arithmetic
    fmt16 = struct.pack("<HHIIHH", 1, 2, sr, sr * 4, 4, 16)
    junk = b"junk" + struct.pack("<I", 0xFFFFFFF8) + b"\0" * 8
    t0 = time.time()
    _upload("REVIEWBAD", _wav_bytes(fmt16, tone[:40000], junk))
    try:
        get_json("/status", timeout=15)
        LOG.say("import: corrupt WAV probed, unit answering after %.1f s" % (time.time() - t0))
    except Exception as exc:                          # noqa: BLE001
        LOG.fail(test, "unit not answering after the corrupt WAV (%s) — SD bus hung?" % exc)
        return
    if switch("Keys", test) == "Keys":
        set_params({"zones": [{"smp": "REVIEWBAD", "root": 48}]}, settle=2.0)   # probe it again
        HEALTH.check("import-bad")
    files = {f["name"]: f for f in get_json("/files", timeout=60)["files"]}
    for n, label in (("REVIEWID3", "ID3 mp3"), ("REVIEWEXT", "extensible 24-bit WAV")):
        f = files.get(n)
        if not f:
            LOG.fail(test, "%s did not import (%s missing from /files)" % (label, n))
        else:
            LOG.say("import: %s -> %s, %s s, %d B" % (label, n, f.get("dur"), f["size"]))
            if not f.get("dur"):
                LOG.fail(test, "%s imported with no duration" % label)
    _req("DELETE", "/files?name=REVIEWBAD")
    HEALTH.check(test)


def t_autosave(mat, reps):
    """4.4 + the AUTOSAVE read failures: a setting made on Keys survives switches."""
    test = "autosave"
    if switch("Keys", test) != "Keys":
        return
    orig = params().get("lvl", 0.85)
    for i in range(reps):
        want = round(0.5 + 0.1 * (i % 4), 2)
        set_params({"lvl": want}, settle=3.0)        # past the 2 s debounce
        for m in ("Tape", "Synth"):
            switch(m, test)
        if switch("Keys", test) != "Keys":
            return
        time.sleep(1.0)
        got = params().get("lvl")
        if got is None or abs(got - want) > 0.011:
            LOG.fail(test, "Keys lvl %.2f did not survive two switches (read %s)" % (want, got))
        HEALTH.check("autosave#%d" % (i + 1))
    set_params({"lvl": orig}, settle=3.0)

LFN = "REVIEW_LONGNAME_TEST_0123456789"               # 31 chars: SAMPLE_ID_LEN - 1


def t_lfn(mat, reboot):
    """6. A 31-char name: upload, load in Keys and Synth, survive a reboot."""
    test = "lfn"
    assert len(LFN) == 31
    sr, n = 44100, 44100
    pcm = b"".join(struct.pack("<hh", s, s) for s in
                   (int(12000 * math.sin(2 * math.pi * 220 * i / sr)) for i in range(n)))
    r = _req("PUT", "/drop_sample", pcm, {"Name": LFN}, timeout=60)
    LOG.say("lfn upload: %s" % r[:120])
    time.sleep(3)
    names = [f["name"] for f in get_json("/files", timeout=60)["files"]]
    if LFN not in names:
        LOG.fail(test, "uploaded name not in /files (got near: %s)"
                 % [x for x in names if x.startswith("REVIEW")])
        return
    if switch("Keys", test) != "Keys":
        return
    set_params({"zones": [{"smp": LFN, "root": 57}]}, settle=2.0)
    got = (params().get("zones") or [{}])[0].get("smp")
    if got != LFN:
        LOG.fail(test, "Keys zone readback '%s'" % got)
    if switch("Synth", test) != "Synth":
        return
    set_params({"wave": LFN}, settle=2.0)
    if params().get("wave") != LFN:
        LOG.fail(test, "Synth wave readback '%s'" % params().get("wave"))
    HEALTH.check(test)
    if not reboot:
        LOG.say("lfn: reboot leg skipped (--reboot to include)")
        return
    time.sleep(6)                                      # autosave debounce
    LOG.say("lfn: rebooting")
    post("/reboot")
    time.sleep(8)
    t_end = time.time() + 60
    while time.time() < t_end:
        try:
            si = get_json("/sysinfo", timeout=5)
            break
        except Exception:                             # noqa: BLE001
            time.sleep(2)
    else:
        LOG.fail(test, "unit did not come back after /reboot")
        return
    HEALTH.rebased()
    time.sleep(4)
    m = machine_now()
    if m != "Synth":
        LOG.note(test, "after reboot the machine is '%s', not Synth" % m)
        switch("Synth", test)
    if params().get("wave") != LFN:
        LOG.fail(test, "after reboot Synth wave is '%s'" % params().get("wave"))
    else:
        LOG.say("lfn: Synth wave survived the reboot")
    HEALTH.check("lfn-reboot")


# ---- main --------------------------------------------------------------------
TESTS = ["switch", "decks", "keys", "tape", "synth", "autosave", "uri", "stall", "import", "lfn"]


def main():
    global IP, LOG, SER, HEALTH, REC
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="stramp.local")
    ap.add_argument("--serial", help="USB serial port; opening it reboots the unit")
    ap.add_argument("--audio", action="store_true", help="record via the Scarlett")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--only", help="comma list of: " + ",".join(TESTS))
    ap.add_argument("--reboot", action="store_true", help="lfn: include the reboot leg")
    ap.add_argument("--seed", type=int, default=None)
    a = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    LOG = Log()
    seed = a.seed if a.seed is not None else int(time.time())
    random.seed(seed)
    IP = socket.gethostbyname(a.host)
    os.environ["STRAEMPLER_IP"] = IP
    LOG.say("unit %s (%s), seed %d, out %s" % (a.host, IP, seed, OUT))

    st = get_json("/status")
    if st.get("recording"):
        sys.exit("the unit is RECORDING — not touching it")

    if a.serial:
        SER = SerialTap(a.serial)
        LOG.say("serial open — the unit reboots; waiting for it")
        time.sleep(10)
        for _ in range(30):
            try:
                get_json("/sysinfo", timeout=4)
                break
            except Exception:                         # noqa: BLE001
                time.sleep(2)
        time.sleep(4)
    REC = Recorder(a.audio)
    HEALTH = Health()
    start_machine = machine_now()
    mat = pick_material()

    only = a.only.split(",") if a.only else TESTS
    for name in only:
        LOG.say("==== %s" % name)
        if SER:
            SER.take(name)
        try:
            if name == "switch":
                t_switch(mat, a.reps)
            elif name == "keys":
                t_keys(mat, a.reps * 3)
            elif name == "tape":
                t_tape(mat, a.reps * 2)
            elif name == "synth":
                t_synth(mat, a.reps * 3)
            elif name == "decks":
                t_decks(mat, a.reps)
            elif name == "autosave":
                t_autosave(mat, a.reps)
            elif name == "uri":
                t_uri(mat, a.reps)
            elif name == "stall":
                t_stall(mat, a.reps)
            elif name == "import":
                t_import(mat, a.reps)
            elif name == "lfn":
                t_lfn(mat, a.reboot)
            else:
                LOG.fail(name, "unknown test")
        except Exception as exc:                      # noqa: BLE001
            LOG.fail(name, "script error: %r" % exc)
            try:
                post("/midi/alloff")
            except Exception:                         # noqa: BLE001
                pass
        HEALTH.check(name)

    try:
        if machine_now() != start_machine and start_machine not in ("", "Stub"):
            switch(start_machine, "restore")
    except Exception:                                 # noqa: BLE001
        pass
    time.sleep(1)
    if SER:
        HEALTH.check("end")
        SER.stop = True

    LOG.say("==== summary: %d fail, %d notes" % (len(LOG.fails), len(LOG.notes)))
    for t, m in LOG.fails:
        LOG.say("  FAIL %-8s %s" % (t, m))
    for t, m in LOG.notes:
        LOG.say("  note %-8s %s" % (t, m))
    if not a.serial:
        LOG.say("  (no --serial: 'leaking'/'still running' are NOT checked)")
    if not a.audio:
        LOG.say("  (no --audio: clicks and load noise are NOT checked)")
    LOG.say("  not covered: Editor/Freesound audition start/stop (panel only)")
    sys.exit(1 if LOG.fails else 0)


if __name__ == "__main__":
    main()
