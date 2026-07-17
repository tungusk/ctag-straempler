# **CTAG Strämpler** — multi-machine fork

A fork of [ctag-fh-kiel/ctag-straempler](https://github.com/ctag-fh-kiel/ctag-straempler)
that turns the Strämpler eurorack module into a **multi-machine instrument**:
one core owns the hardware (audio transport, 8 CV in, 2 gates, SD sample pool,
display, WiFi/REST) and hosts swappable "machines" selected at runtime.
Active development is on the [`v09-machines`](../../tree/v09-machines) branch.

## 📖 Machine documentation

**[docs/machines](docs/machines/README.md)** — one page per machine with live
screenshots, feature breakdowns, and control tables, plus the
[common concepts](docs/machines/common.md) (clock/grid system, sample pool,
encoder grammar, web interface, broadcast/bounce).

| Machine | One-liner |
|---|---|
| [Sampler](docs/machines/sampler.md) | Two-voice clock-synced loop recorder — CV matrix, quantized crop windows |
| [Looper](docs/machines/looper.md) | 4-track clock-synced RAM looper |
| [Slicer](docs/machines/slicer.md) | Any-length slicer (grid / transient / `.ot`), streamed tails |
| [Granular](docs/machines/granular.md) | 16-grain cloud |
| [Glitch](docs/machines/glitch.md) | Live-input stutter / beat-repeat |
| [Drums](docs/machines/drums.md) | 4 pads × 2 choke layers, master DJ filter, per-pad reverb sends |
| [Deck](docs/machines/deck.md) | Tempo-syncing track player — PLL-locked varispeed, exact BPM analysis |
| [DoubleDecker](docs/machines/dualdeck.md) | Two synced decks + crossfader + per-deck DJ filters |
| [Tracker](docs/machines/tracker.md) | MOD/XM/IT module player (libxmp) with performance loop + scrub |
| [Freesound](docs/machines/freesound.md) | freesound.org search / preview / import from the browser |
| [Radio](docs/machines/radio.md) | Internet radio (icecast MP3, any rate, live-resampled) |
| [Synth](docs/machines/synth.md) | Mono VA/FM/wavetable voice — 1V/oct, ADSR, filter, LFO, patches |
| [Keys](docs/machines/keys.md) | Tonal instrument sampler — pitched playback with sustain loop |
| [Tape](docs/machines/tape.md) | Single-track tape recorder/editor — big-waveform crop UI, FX printed to tape |
| [Editor](docs/machines/editor.md) | Web-driven batch file editor (normalize / reverse / fade / trim) |

## Web + network features

The module serves its own web page: file manager, universal upload with
convert-on-import (MP3 / any rate / any depth → native), a **teleremote**
(live meters, CV fader console, soft triggers, encoder, machine switching,
settings editor, live TFT **screen mirror**), and per-machine tabs
(Radio, Editor, Freesound).

- **Broadcast** (port 8000): the live output — or the line *input* — as
  endless WAV or 96k MP3, straight into a browser or VLC.
- **Icecast push**: the module can act as a source client, streaming its
  output to an icecast mountpoint.
- **Bounce**: record any machine's output to a new pool take from the browser.
- **OTA updates**: `tools/ota.sh` flashes over WiFi in ~14 s, with rollback.

Build and hardware notes live in [CLAUDE.md](CLAUDE.md); flashable snapshots
of every milestone are in [`bin/`](bin/).

## Hardware

**Building the module?** See the companion
[strampler-build-pack](https://github.com/tungusk/strampler-build-pack):
Eagle + KiCad board files for the Antumbra 18 HP redesign, BOMs with 2026
EOL substitutions, and fab/assembly + DIY-kit ordering guides.

Unit-specific corrections in this firmware (CV channel mapping, antenna power)
are documented in [CLAUDE.md](CLAUDE.md).

## Upstream, licenses & credits

This project stands on the original CTAG Strämpler by the
[CTAG team at Kiel University of Applied Sciences](https://www.creative-technologies.de)
(Robert Manzke, Phillip Lamp, Niklas Wantrupp, with panel design support by
David Knop / instruments of things) and [Antumbra](http://www.antumbra.eu/).
The original project README is preserved in
[README.upstream.md](README.upstream.md). A generic display fix from this fork
was upstreamed as [PR #29](https://github.com/ctag-fh-kiel/ctag-straempler/pull/29).

**Licenses** (inherited from upstream, unchanged):
- Hardware: [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)
- Software: see the [LICENSE](LICENSE) file; vendored components keep their
  own licenses ([shine](components/shine/COPYING) is LGPL, libxmp is MIT)
- Freesound: [API terms](https://freesound.org/docs/api/terms_of_use.html) —
  and mind the license of each sound you use (shown in the sound browser)
