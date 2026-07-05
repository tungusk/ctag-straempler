# m0-complete firmware (2026-07-04)

Snapshot of `v09-machines` at commit `4f35d05` (tag `m0-complete-20260704`).
Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0. Hardware-verified
(RTS soak 15/15, autosave round-trip, encoder + CV mapping checked by ear/hand).

Machine architecture phase M0 complete:
- M0a/M0b: audio path runs through the machine interface; sampler engine lives
  in components/machine_sampler (core audio.c is an 88-line shell)
- M0c: machine-owned menu registration; all sampler menu + display code moved
  out of core (sampler_menu.c, sampler_tft.c)
- M0d: seams severed — sampler owns its param queues, machine-agnostic
  autosave container in AUTOSAVE.JSN ({"machine":name,"state":{...}}),
  sampler-less proof build links (tools/proof_build.sh)

Unit-specific + UX fixes on top:
- CV: removed upstream spi_per.c 5/6 ADC swap (wrong for this unit's PCB —
  it crossed panel knobs 6/7)
- Encoder: quadrature state-machine decode, one event per detent (old decoder
  ate every second click on this both-rest-states encoder)
- Encoder acceleration on the 0..800 Q13.3 percent params (1/4/16 steps by
  click speed)

Known-open: intermittent white-screen display issue (all firmwares, hardware
suspected, reboot recovers); CV ch4 stuck low (never map modulation to it),
knob 5 dead, knob 8 half-range.

## Flash

```bash
./flash.sh                          # uses /dev/cu.usbserial-3110 by default
./flash.sh /dev/cu.usbserial-XXXX   # or specify port
```
