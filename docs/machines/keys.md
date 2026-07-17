# Keys

**Tonal instrument sampler** — load one sample, tell it the root note, and
play it pitched over ±2 octaves from 1V/oct with a **sustain loop** so held
notes ring for as long as the gate does. The Synth's envelope/filter/reverb
architecture wrapped around a sample.

![Keys live](img/keys-live.png)
![Keys setup](img/keys-setup.png)

## Features

- Resident mono sample (cubic-interpolated varispeed — clean high end).
- **Forward sustain loop**: Loop Start/End with zero-crossing snap and a
  wrap **crossfade** — dial in a click-free sustain region and hold notes
  indefinitely; releases play through naturally.
- Synth-style **ADSR** + envelope-opened SVF low-pass; glide; quantize.
- **Four macro knobs** with takeover: K5 start offset, K6 cutoff, K7
  resonance, K8 env→cutoff — mirrored as live dials on the dashboard.
- **CV matrix** with sampler-native destinations: cutoff, res, env>cut,
  level, pitch, **start**, **loop move**, **loop length**.
- **Reverb**; **named patches** (`Save/Load Patch`, newest-first browser).
- Live dashboard: waveform strip with the loop window boxed and a playhead,
  note name readout, macro dials, ADSR curve.

## Controls

| Control | Function |
|---|---|
| CV1 | 1V/oct pitch |
| TR1 | Gate (retriggers from the start offset) |
| Knobs 5–8 | start / cutoff / res / env>cut (takeover) |
| Encoder long | Live ↔ Setup |

## Setup

Load Sample, Root Note, Base Note, Quantize, Loop Mode/Start/End/Xfade,
ADSR, Env>Cut, Glide, Level, Reverb + Mix, CV Matrix,
Save Patch / Load Patch.
