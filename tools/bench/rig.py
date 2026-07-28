#!/usr/bin/env python3
"""Shared bench rig: capture the module's ANALOG OUTPUT and drive it over REST.

Why analog capture rather than /bounce or the :8000 broadcast — both of those put
load INSIDE the thing under test. A bounce's SD writes alone pushed peak block
cost to 1447 us of a 1450 us budget even on a DRY case, and the broadcast runs an
encoder next to the audio task. Recording off the interface measures the module
without measuring the measurement, and it captures what Arlo actually hears,
codec included.

Sibling of tools/analyze_drift.py (same shape: multi-channel capture, offline
numpy). Requires sox with coreaudio, and numpy.

  from rig import Rig
  r = Rig()
  r.check_device()
  wav = r.capture(6.0, "take.wav")
  x, sr = load_wav(wav)
"""
import json
import os
import subprocess
import time
import urllib.request
import wave

import numpy as np

IP = os.environ.get("STRAEMPLER_IP", "192.168.3.227")
# sox needs a moment to open the device before it is really recording. Both the
# calibration and the soak MUST wait the same amount, or the wall-clock-to-
# recording-time offset measured by one does not apply to the other.
CAPTURE_OPEN_S = 0.6
# Breathing room between closing one capture and opening the next. Without it,
# a day of 30 s segments wedges CoreAudio's link to the interface — sox reports
# 0% input, records nothing, never exits, and only a physical replug clears it.
CAPTURE_COOLDOWN_S = 0.8
RATE = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
CALIB_PATH = os.path.join(HERE, "calib.json")


# ---- transport ---------------------------------------------------------------
# Retry the HTTP, not the measurement: a 40 s capture lost to one dropped packet
# costs the whole take, and the laptop's wifi dropped twice mid-run on 2026-07-26
# (OSError 50). Device-side peak-holds accumulate regardless, so a missed poll
# only costs resolution, never an event.
def _open(req, timeout, tries=4):
    for i in range(tries):
        try:
            return urllib.request.urlopen(req, timeout=timeout).read()
        except Exception as exc:                      # noqa: BLE001 - any transport fault
            if i == tries - 1:
                raise
            print("   [retry %d: %s]" % (i + 1, exc), flush=True)
            time.sleep(1.0 + i)
    return None


def get(path, timeout=10):
    return _open("http://%s%s" % (IP, path), timeout)


def post(path, body=None, timeout=15):
    req = urllib.request.Request("http://%s%s" % (IP, path),
                                 data=(body.encode() if body else b""), method="POST")
    return _open(req, timeout)


# ---- wav ---------------------------------------------------------------------
def load_wav(path):
    """-> (samples[frames, channels] float in -1..1, samplerate).

    Handles the 24-bit packing python's `wave` hands back as raw bytes. Note
    captures MUST be written with `-t wavpcm`: sox's default coreaudio output is
    WAVE_FORMAT_EXTENSIBLE (tag 65534) and `wave` refuses to parse it.
    """
    with wave.open(path) as w:
        n, ch, width, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(n)
    # A capture that was cut short (killed sox, full disk, unplugged interface)
    # ends mid-frame and the raw byte count is not a whole number of frames.
    # Truncate rather than raising: a 99%-complete take is still worth analysing,
    # and losing an hour of soak to a ragged last frame would be absurd.
    stride = width * ch
    if len(raw) % stride:
        raw = raw[:len(raw) - (len(raw) % stride)]
    if width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / 8388608.0
    elif width == 2:
        v = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif width == 4:
        v = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError("unsupported sample width %d" % width)
    return v.reshape(-1, ch), sr


def dbfs(x):
    a = float(np.max(np.abs(x))) if len(x) else 0.0
    return 20.0 * np.log10(a) if a > 0 else -999.0


# ---- analysis helpers --------------------------------------------------------
# These exist because every one of them was got WRONG by hand on 2026-07-26 and
# produced a confident, published, incorrect number.
SILENCE_FLOOR = 0.005          # ~-46 dBFS: below this a "take" is noise


def assert_signal(x, label="take"):
    """A take that is silence will still yield a fundamental, a modulation depth
    and a click count — all of them garbage. One batch of conclusions came from
    captures at -66 dBFS where the window detector locked onto the noise floor
    and reported a 60 Hz 'fundamental' (it was mains hum). Check first, always."""
    pk = float(np.max(np.abs(x)))
    if pk < SILENCE_FLOOR:
        raise RuntimeError("%s is SILENT (peak %.5f, %.1f dBFS) — nothing to analyse"
                           % (label, pk, dbfs(x)))
    return pk


def note_window(x, sr, skip_head=0.25, skip_tail=0.25, thresh=0.3):
    """Slice out the part where the note actually SOUNDS.

    Measuring a whole file mixes in the reverb tail filling and the silence after
    the note, and both corrupt every statistic downstream: the tail ramp alone
    read as 49% 'modulation' on a case whose settled value is 16%, and invented a
    pitch trend that vanished once it was excluded.
    """
    assert_signal(x, "window input")
    k = max(1, int(0.02 * sr))
    env = np.sqrt(np.mean((x[:len(x) // k * k].reshape(-1, k)) ** 2, axis=1))
    t = np.arange(len(env)) * (k / sr)
    on = t[env > thresh * env.max()]
    if len(on) < 2:
        raise RuntimeError("no sustained note found in the capture")
    a, b = on[0] + skip_head, on[-1] - skip_tail
    if b <= a:
        raise RuntimeError("note too short to window (%.2f s)" % (on[-1] - on[0]))
    return x[int(a * sr):int(b * sr)], a, b


def fundamental(x, sr, lo=40.0, hi=2000.0, n=1 << 16):
    """Parabolic-refined spectral peak. Use ONCE on a reference take and PIN the
    result across a comparison set — detecting per take put one take at 115.7 Hz
    and made it look 96% inharmonic when it was simply playing flat."""
    seg = x[:n] if len(x) >= n else x
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n))
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    i0, i1 = np.searchsorted(freqs, lo), np.searchsorted(freqs, hi)
    k = i0 + int(np.argmax(spec[i0:i1]))
    y0, y1, y2 = spec[k - 1], spec[k], spec[k + 1]
    denom = y0 - 2 * y1 + y2
    delta = 0.5 * (y0 - y2) / denom if denom != 0 else 0.0
    return float((k + delta) * sr / n)


# ---- the rig -----------------------------------------------------------------
class Rig:
    def __init__(self, device=None, rate=RATE):
        self.device = device or self.find_device()
        self.rate = rate
        self.calib = self.load_calib()

    # -- interface
    @staticmethod
    def find_device():
        out = subprocess.run(["system_profiler", "SPAudioDataType"],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            s = line.strip().rstrip(":")
            low = s.lower()
            if ("scarlett" in low or "focusrite" in low) and "source" not in low:
                return s
        raise RuntimeError("no Focusrite/Scarlett input found — is it plugged in?")

    def check_device(self):
        print("input device: %s @ %d Hz" % (self.device, self.rate))
        return self.device

    def capture(self, seconds, path, channels=2):
        """Record stereo. `-t wavpcm` is required (see load_wav)."""
        proc = subprocess.Popen(
            ["sox", "-t", "coreaudio", self.device,
             "-b", "24", "-e", "signed-integer", "-t", "wavpcm",
             "-r", str(self.rate), "-c", str(channels),
             path, "trim", "0", str(seconds)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        return proc

    def capture_blocking(self, seconds, path, channels=2, tries=3):
        """Capture, detecting a WEDGED device instead of blocking on it.

        Opening and closing the interface hundreds of times in a day wedges
        CoreAudio's link to it: sox still starts and reports 0% input, records
        nothing, and never exits. It happened after a few hundred captures and
        needed the interface physically replugged. So: watch the output file
        grow, give up early if it does not, and cool down between opens rather
        than hammering the device.
        """
        for attempt in range(tries):
            proc = self.capture(seconds, path, channels)
            deadline = time.time() + seconds + 15
            stalled = False
            time.sleep(min(2.0, seconds))
            last = -1
            while proc.poll() is None and time.time() < deadline:
                size = os.path.getsize(path) if os.path.exists(path) else 0
                if size == last and size < 1000:      # open but delivering nothing
                    stalled = True
                    break
                last = size
                time.sleep(1.0)
            if proc.poll() is None:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except Exception:                     # noqa: BLE001
                    pass
                stalled = True
            time.sleep(CAPTURE_COOLDOWN_S)            # let CoreAudio settle
            if not stalled:
                return path
            print("   [capture stalled, attempt %d/%d]" % (attempt + 1, tries), flush=True)
            time.sleep(2.0)
        raise RuntimeError("the audio interface is WEDGED — unplug and replug it "
                           "(sox opens the device but receives no samples)")

    # -- calibration
    def load_calib(self):
        if os.path.exists(CALIB_PATH):
            with open(CALIB_PATH) as fh:
                return json.load(fh)
        return None

    def save_calib(self, data):
        with open(CALIB_PATH, "w") as fh:
            json.dump(data, fh, indent=2)
        self.calib = data

    # -- device control
    @staticmethod
    def status(fx=False):
        return json.loads(get("/status?fx=1" if fx else "/status"))

    @staticmethod
    def params():
        return json.loads(get("/remote/params"))

    @staticmethod
    def set_params(patch, verify=True, settle=1.4):
        """POST a patch and READ IT BACK.

        /remote/params is a PARTIAL preset — preset_load only overwrites the keys
        present, so anything omitted silently keeps whatever a previous test left
        behind. An `odbs=80` left over from an earlier run muted the overdrive
        through an entire later sweep before this readback existed.
        """
        post("/remote/params", json.dumps(patch))
        time.sleep(settle)
        if not verify:
            return None
        back = Rig.params()

        def differs(want, got):
            # The module keeps params as float32 and JSON hands them back at full
            # double precision, so 0.005 returns as 0.00499999988824129. Comparing
            # with != flagged five such non-differences on every single call, which
            # is how a reader learns to ignore this line — and then misses the ones
            # that matter (a physical knob refusing a remote write).
            if isinstance(want, bool) or isinstance(got, bool):
                return bool(want) != bool(got)
            if isinstance(want, (int, float)) and isinstance(got, (int, float)):
                return abs(float(want) - float(got)) > max(1e-4, abs(float(want)) * 1e-4)
            return want != got

        bad = {k: (v, back.get(k)) for k, v in patch.items()
               if isinstance(v, (int, float, str, bool)) and differs(v, back.get(k))}
        if bad:
            print("   [params did not apply: %s]" % bad, flush=True)
        return bad

    @staticmethod
    def trigger(ms=200, trig=1):
        """One gate pulse, returning immediately. Distinct from hold_gate, which
        blocks and re-posts: calling hold_gate with a short duration fires TWICE
        1.2 s apart and reports the wrong timestamp, which made the soak unable
        to account for its own notes."""
        post("/remote/trig?t=%d&ms=%d" % (trig, max(5, min(2000, int(ms)))))

    @staticmethod
    def hold_gate(seconds, trig=1):
        """Hold the gate open. /remote/trig sets an absolute until-tick, so
        re-issuing EXTENDS the hold without retriggering; 2000 ms is the cap the
        endpoint enforces, hence the ~1.2 s refresh."""
        t0 = time.time()
        post("/remote/trig?t=%d&ms=2000" % trig)
        while time.time() - t0 < seconds:
            time.sleep(1.2)
            post("/remote/trig?t=%d&ms=2000" % trig)

    # -- MIDI note hold. PREFER THIS over hold_gate for anything whose LEVEL or
    # continuity is being measured. The soft gate behind /remote/trig releases
    # early and intermittently (2026-07-27: a 30 s hold survived 8 s once and
    # 2.3 s another time, with the module's own deadline verifiably set to 3000
    # ticks) — as a level reference that reads as a signal wandering by 20 dB for
    # no reason, which is exactly what it did before this existed.
    #
    # The MIDI gate has its own 5 s liveness timeout (audio_midi_gate compares
    # against s_midi_seen), and note_on is what stamps it — so re-posting the
    # SAME note is the heartbeat. It does not retrigger: the machine sees the
    # gate stay continuously high, so the voice's env never re-enters attack.
    @staticmethod
    def note_on(note=57):
        post("/midi/on?note=%d" % int(note))

    @staticmethod
    def notes_off():
        post("/midi/alloff")

    @staticmethod
    def hold_note(seconds, note=57):
        """Hold one MIDI note for `seconds`, refreshing the liveness stamp."""
        t0 = time.time()
        Rig.note_on(note)
        while time.time() - t0 < seconds:
            time.sleep(1.4)
            Rig.note_on(note)
        Rig.notes_off()


if __name__ == "__main__":
    r = Rig()
    r.check_device()
    st = r.status()
    print("module: %s  aus %s  ausgap %s  sav %s"
          % (st.get("machine"), st.get("aus"), st.get("ausgap"), st.get("sav")))
    print("calibration: %s" % ("loaded" if r.calib else "NONE — run calib.py"))
