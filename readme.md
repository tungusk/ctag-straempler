# **CTAG Strämpler** — multi-machine fork

This is a fork of [ctag-fh-kiel/ctag-straempler](https://github.com/ctag-fh-kiel/ctag-straempler)
that turns the Strämpler into a **multi-machine instrument**: the core owns
the hardware (audio transport, CV, SD, display, WiFi) and hosts swappable
"machines" selected at runtime via System → Machine. Active development is
on the [`v09-machines`](../../tree/v09-machines) branch (July 2026).

**The machines** (one active at a time, per-machine autosave):

| Machine | What it does |
|---|---|
| **Sampler** | The original v0.9 dual-voice streaming sampler |
| **Sampler2** | Extended fork: recording, crop mode, signed CV matrix amounts, CV-addressable start/length |
| **Looper** | 4-track clock-synced RAM looper with save-to-library and per-track filter |
| **Slicer** | Grid or transient slicing of a sample, CV-addressable slices |
| **Granular** | 16-grain cloud over a mono sample |
| **Glitch** | Live-input stutter/beat-repeat with clock sync |
| **Drums** | 8-pad CV-triggered drum sampler: routable trigger inputs with floor-tracking detection, per-pad level/pan/decay, CV-select mode |
| **Deck** | Tempo-syncing track player: streams a long SD sample varispeed, phase-locked to an external CV clock (pulse-level PLL, clock mult/div), with offline BPM + beat-grid detection cached per track. Encoder scrubs by the bar; TR1 restarts at the downbeat, TR2 stops. *(active development)* |
| **Freesound** | Web-driven freesound.org search + preview download straight into the sample library; direct MP3-URL import |

**Web features** (the module serves its own page):
- **Teleremote**: live CV/gate monitor, encoder control, soft trigger pulses,
  machine switching, and a machine-settings editor — all from the browser
  (on-device kill switch in System → Settings). 
- **Universal upload**: drop any audio file (wav/mp3/flac/ogg) — converted
  in-browser to the native RAW format.
- File manager with download/delete, streaming-safe SD access.

Plus core fixes along the way: SD-bus serialization (`sd_lock`), low-memory
web fixes, HTTPS cert-bundle repair (freesound access had been silently
broken since IDF 4.3), a decades-old display-corrupting stack smash
(upstreamed as [PR #29](https://github.com/ctag-fh-kiel/ctag-straempler/pull/29)),
and declick/trigger-detection work throughout. Build docs live in
[CLAUDE.md](CLAUDE.md); flashable snapshots of each milestone are in
[`bin/`](bin/) (latest: `drums-remote-v1`).

**Building the hardware?** See the companion
[strampler-build-pack](https://github.com/tungusk/strampler-build-pack):
Eagle + KiCad board files for the Antumbra 18 HP redesign, BOMs with 2026
EOL substitutions, and fab/assembly + DIY-kit ordering guides.

---

*Original upstream README follows.*

This repository contains the firmware and the hardwaredesigns (CTAG and [Antumbra](http://www.antumbra.eu/)) of Strämpler.

!!**NEW**!! Strämpler firmware can now be built in the cloud using Github actions
!!**NEW**!! Strämpler can now run [CTAG TBD](https://github.com/ctag-fh-kiel/ctag-tbd) as alternative firmware! Enclosed in /bin folder!

![Strämpler harware UI](https://raw.githubusercontent.com/wiki/ctag-fh-kiel/ctag-straempler/img/straempler-boot.jpg)



## What it is:
- Half streamer, half sampler, therefore called Strämpler (close to German word of Strampler meaning romper suit).
- Allows streaming of large audio files from SD card (limit 2GB file size due to FAT32 used).
- A eurorack modular synth module with 22 HP width and an internet connection.
- A bridge to connect to [freesound.org](https://freesound.org), to play with samples within your modular synth setup.
- Allows tweaking of your sound just like you'd do with a sampler + *a modular synth*.
- Allows to parameter tweak and modulate sounds by control voltage (CV), employing a complex modulation matrix.

## Why it is:
- A group of audio enthusiasts enjoying coding and hardware making.
- The sexiness of sampling for sound design and synthesis.
- The (subjective) need to have more sampling modules in the eurorack modular synth domain.
- The (subjective) need to make eurorack more compatible with internet of things.
- Build a platform to learn, build and practise skills, and engage students.
- To allow anyone to understand technology by offering open access.
- To benchmark the capabilities of the WiFi/BLE enabled [Espressif ESP32](https://docs.espressif.com/projects/esp-idf/en/latest/#) platform and get a deep understanding of it.
- To squeeze and optimize code so that it can work on a small embedded system.
- Because we can.
- Because of some inspiration of the [Elektron Octatrack](https://www.elektron.se/products/octatrack-mkii/).

## Features: 
- 2 voice eurorack sample streaming module.
- Streaming of large sound files from SD-card (limited by FAT32 2GB).
- Internet connection to [freesound.org](https://freesound.org), allows to download files through freesound.org api onto SD-card of module.
- One ADSR per voice to control sample amplitude.
- One band pass filter per voice with controllable base, width and Q. Can be used as low / high pass (biquad implmentation).
- Arbitrary playback speed adjustment (+/- 100%). 
- Pitching +/- 12 halftones, samples can be played e.g. using external gate / CV keyboard.
- Distortion per voice (tanh() saturation).
- Delay as send effect (stereo, ping pong, max delay time 1.5s).
- External stereo input, with delay send, mix with voices.
- Modulation matrix, where many parameters can be modulated using external CV.
- Gate and latch modes for sample playback.
- REST-API for user file upload (service discovery by MDNS / bonjour).
- 44.1kHz, 32 bit float internal resolution, 24 bit codec resolution, 12 bit CV input resolution sampled at 2KHz (yes, modulation in the audio range is possible to a certain extend), approx. 1ms DSP buffer latency  (32 words per channel).
- Approx. 100mA +12V / 10mA -12V power draw.

## Potential new features / current limitations / work to be done:
- Sampling of external input. 
- Upload to freesound.org.
- Improved sound browser, browse by tag, browse by search.
- More testing of modulation.
- More effects.
- More performance optimization.
- Automatic voice alternation.
- Bug identification and fixing.
- Code refactoring to make things more beautiful.
- More user friendly interaction.
- Documentation / tutorials.
- Your ideas?

## How to engage yourself:
- Join the [enthusiastic developer](https://codewithoutrules.com/2018/11/12/enthusiasts-vs-pragmatists/) team on Github.
- Help build and spread the hardware module (and the word).
- Use Strämpler to create cool sounds and music and share them (with a link to us).
- More ideas?

## Words of caution: 
- CTAG Strämpler does contain bugs, it comes without warranty of any kind, none of the authors are liable to damages arising by the use of it.
- CTAG Strämpler is an intermediate to advanced project, both in terms of hardware and software design:
    - In order to build the hardware you need intermediate to advanced SMD soldering skills and respective tools. I.e. the PCB contains many 0604 SMD components as well as TSSOP packages and a QFN. However, with a bit of practise you will be able to DIY build your own module. Dare and be rewarded, tackle the frustration on the path, it's worth it. The idea of the platform is also to boost your soldering skills, it *CAN* all be done by hand.
    - The software is built using [Espressif IDF](https://esp-idf.readthedocs.io) using the C programming language. Intermediate knowledge of C is required to understand the code. Furthermore, some basic DSP algorithms are applied. A DSP newbie, however, could take the project to really get rolling and build up on DSP capabilities. CTAG Strämpler is ideal to try out and play with your own DSP algorithms.
- The [Espressif ESP32](https://en.wikipedia.org/wiki/ESP32) platform used for CTAG Strämpler is, with regard to its computational power and I/O capabilities, hard at its limit. We're squeezing the platform here and are already surprised, what one can get out of $5 chip in the year 2019. Surely, other DSPs / microcontrollers could do a better job, but do they allow for internet of things as easily?

## How to get started / user manual:
See the [Wiki pages](https://github.com/ctag-fh-kiel/ctag-straempler/wiki) of this project.

## Build instructions
The easiest way is to create a fork on Github from this repository. Then make sure GitHub Actions are activated for your fork by going to the corresponding tab at your fork and clicking on the green button.
Every time you push new commits into your fork (master or dev branch) the action will build a new draft release, which will upon successful run of the action be available at your fork's release tab.

If you want to build on your own system, you can use a virtual development environment image with Docker (see here for [ESP IDF Docker instructions](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-docker-image.html)). 
Here are some steps:
- Install [Docker](https://www.docker.com/)
- Install [Git](https://git-scm.com/)
- Open a command line shell
- Clone the Strämpler repository with ```git clone https://github.com/ctag-fh-kiel/ctag-straempler.git``` and change into it with ```cd ctag-straempler```
- Build the firmware with ```docker run --rm -v $PWD:/project -w /project rma31/idf:v4.1-strampler idf.py build```

You can edit the source files using any IDE / text editor you like.
## Flash instructions
- Either download the firmware from the Github release tab or use your own
- Install [ESP Tool](https://github.com/espressif/esptool)
- Adapt the [Firmware flash script](https://github.com/ctag-fh-kiel/ctag-straempler/blob/master/bin/flash.sh) to your system needs and flash the firmware

## Hardware
All hardware design can be found [https://github.com/ctag-fh-kiel/esp32-eurorack-audio](https://github.com/ctag-fh-kiel/esp32-eurorack-audio).

## Links
- [Demonstration videos on YouTube](https://youtu.be/zmj8tKPHV8g).

## Licenses:
- Hardware is licensed under [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/).
- Software licenses, see [license](LICENSE) file.
- Freesound license: [Freesound.org API terms](https://freesound.org/docs/api/terms_of_use.html) and [here](https://freesound.org/help/tos_api/) as well as the [freesound website terms of use](https://freesound.org/help/tos_web/).
- Don't forget to obey the license of the respective sound files used in your music. License details are shown for the individual sound file in your CTAG Strämpler sound browser, including the sound file authors name.

## Who made this happen:
- The team of [CTAG (Creative Technologies Arbeitsgruppe) at Kiel University of Applied Science](https://www.creative-technologies.de), including:
    - Robert Manzke
    - Phillip Lamp
    - Niklas Wantrupp
    - With kind support for the front panel design by David Knop from [instruments of things](http://www.instrumentsofthings.com/).
- [Antumbra](http://www.antumbra.eu/)
- More people like you :)
