# Platform strategy — where this goes next (2026-07-15, marathon reflection)

Durable capture of a strategic thread Arlo and Claude worked through mid-marathon.
NOT an immediate build plan — this is direction, so it survives context resets.

## This device's identity: "the brainless voice you patch INTO"
The hardware has **no CV/gate OUT** and the audio outs are **AC-coupled** — a held
voltage (1V/oct pitch, filter CV) is physically impossible, not just
unimplemented. So this module must NOT pretend to sequence *other* modules.

Nuance worth knowing: AC coupling kills sustained levels but *passes edges*, so a
trigger/clock/gate *can* technically go out an audio jack as a pulse — but it's
hacky/marginal (line level ~1–2 V vs Eurorack triggers wanting ~5 V, burns an
audio channel). Not a real feature; just note the line between "impossible" (CV
out) and "ugly but possible" (trigger out).

**Coherent identity:** an excellent PLAYER + SOUND GENERATOR + EFFECT that *other*
sequencers drive. Its integration story is the INPUT side — 8 CV + 2 gate in + the
CV matrix. Lean into that; it isn't missing anything as a voice.

**Internal sequencing is fine** (drum patterns, recorded mod-lanes driving its own
machines) — needs no CV out. Legit low-priority fun feature.

## Sequencer conclusion
Leave sequencing OF OTHER MODULES to dedicated devices. Don't build an on-device
sequencer aimed at external CV/gate — the platform can't deliver it.

## The fork: expander vs. new platform
- **Expander sub-module** (DAC + op-amp to ±5–10 V + GPIO gates) extends THIS
  instrument. Feasible (Arlo has a hardware track + PCBWay), but bolts CV-out onto
  a platform frozen at IDF 4.3 (crackle regression) with academic-fork cruft +
  LGPL/vendoring friction (Shine, helix).
- **New platform** sheds all of it and buys NATIVE CV/gate I/O. Target:
  **Electrosmith Daisy (STM32H750)** — purpose-built for this module class (codec +
  CV in/out + 64 MB SDRAM + modern toolchain). ESP32-S3 on IDF 5.x is the
  stay-in-family alternative (USB + more RAM) but still needs custom analog for CV.

## The reframe that de-risks everything
The work we're doing NOW is **platform-portable R&D**. The operability blueprint —
4-macro-knob + takeover, Live settings dashboard, CV matrix, patch save/load — is
NOT ESP32-specific. On a new platform it becomes the design spec. Nothing is wasted
even in the start-over scenario. (Same insight as: the synth is the blueprint for
the tonal instrument sampler — see [[instrument-sampler-design]].)

## Can Claude design the hardware? (honest calibration)
- **Code**: strong, verifiable in seconds. **Circuits**: decent-to-good, but the
  loop is slow + costly (a wrong part = a $100 spin + weeks), so error rate matters
  far more.
- **Strong at**: system architecture, canonical Eurorack analog (CV in/out, gates,
  ±12 V power — all published/solved, e.g. open-source Mutable + Daisy refs), and
  firmware (libDaisy/DaisySP C++ = same work as now, roster ports over).
- **Weak/risky at**: physical PCB LAYOUT (routing, ground planes, analog/digital
  separation, noise) and fine analog tails (op-amp stability, noise floor). Can't
  solder/scope/probe.
- **Other LLM?** Not a different chatbot — peers at schematic reasoning, all weak at
  layout. Real leverage = reference designs + KiCad + fab DFM check + optionally an
  EDA-integrated copilot (Flux.ai/JITX) for the LAYOUT step + a human board review.

## Attainable path (if/when greenfield)
1. **Daisy Patch SM** (`Patch.Init()` SOM): brings out conditioned CV in/out, gates,
   codec, MCU, RAM. Custom board becomes a SIMPLE carrier (jacks/pots/LEDs/power) —
   collapses the risky analog to near-zero.
2. **Prototype in firmware on a stock Daisy dev board FIRST** — zero custom hardware.
   Port the blueprint + DSP, prove the UX. THEN spin a custom Eurorack PCB only once
   software/UX are locked. Sequences risk correctly: firmware (strength) first + free,
   the board spin (slow/costly) last + only once proven.

**Verdict: high feasibility via Daisy-SOM + firmware-first.** Genuinely the next
project. Arlo's module ideas are the fuel. No decision needed today; must not stall
the current marathon (which is harvesting real value from this device as a voice).

## Selectable bootloader — OTA shipped, boot MENU shelved (2026-07-15)
OTA + rollback are DONE (two 3 MB slots, `tools/ota.sh`, auto-revert; the
churn-killer). The **boot menu** (hold TR1+TR2 → Normal / pick-slot / Safe-mode /
Factory-reset) is **shelved on purpose**: with only one firmware it's clutter — keep
the bootloader invisible until a SECOND bootable image exists to select between.
Revisit when an 8 MB-adapted TBD (or other firmware) is ready to load into ota_1.
TBD itself is shelved (16 MB design vs this 8 MB unit + no CV-out; working fork
build archived at `~/ctag-straempler-backups/tbd-strampler-cloud-16mb/`).
