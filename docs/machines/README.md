# Strämpler Machines

The Strämpler firmware hosts swappable **machines** — independent instruments
sharing one core (SD sample pool, TFT + encoder, WiFi/REST, 8 CV in, 2 trig
in, stereo line in/out). One machine runs at a time; switch via
the **Machine** affordance (top right of any Setup page) on the device, or the
web page. Each machine
remembers its settings independently (autosave) across switches and reboots.

Shared conventions live in **[Common concepts](common.md)** — the CV matrix,
the module-wide clock, the sample pool, the browser, knob takeover, the web
page, and the broadcast/bounce services.

| Machine | One-liner |
|---|---|
| [Sampler](sampler.md) | Two-voice clock-synced loop recorder with CV matrix and crop windows |
| [Looper](looper.md) | 4-track clock-synced RAM looper with per-track filters |
| [Slicer](slicer.md) | Any-length sample slicer — grid, transient, or `.ot` slices, streamed tails |
| [Granular](granular.md) | 16-grain cloud over a mono sample |
| [Glitch](glitch.md) | Live-input stutter / beat-repeat, clock-synced |
| [Drums](drums.md) | 4 pads × 2 choke layers, one-shot drum sampler with master filter |
| [Deck](deck.md) | Tempo-syncing track player — varispeed phase-locked to external clock |
| [DoubleDecker](dualdeck.md) | Two synced decks with crossfader and per-deck DJ filters |
| [Tracker](tracker.md) | MOD/XM/IT module player (libxmp) with live loop + scrub |
| [Freesound](freesound.md) | Freesound.org search / preview / download, on the panel or the web |
| [Radio](radio.md) | Internet radio — icecast MP3 streams, any rate, resampled |
| [Synth](synth.md) | Mono subtractive/FM/wavetable voice — 1V/oct, ADSR, filter, LFO, reverb |
| [Keys](keys.md) | Tonal instrument sampler — pitched sample playback with sustain loop |
| [Tape](tape.md) | Single-track tape recorder/editor — big-waveform crop UI, FX-in-path |
| [Editor](editor.md) | Sample editor — normalize / reverse / fade / trim, audition with TR1 |

*Screenshots are pulled from the live device's shadow framebuffer
(`/screenshot`). Refresh them any time with `tools/capture_docs.sh <ip>`.*
