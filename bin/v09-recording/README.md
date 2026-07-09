# v09-recording firmware

Built with IDF 4.3 + xtensa-esp32-elf esp-2021r2-patch3-8.4.0. Clean audio on both voices confirmed.

## Flash

```bash
chmod +x flash.sh
./flash.sh                          # uses /dev/cu.usbserial-3120 by default
./flash.sh /dev/cu.usbserial-XXXX  # or specify port
```

## Changelog (vs v0.9 base)

- **Recording toggle + auto-load** — record to SD card, file auto-loads into the triggering slot on stop
- **Colored transport bars** — blue (idle), green (playing), yellow (armed), red (recording)
- **Recording menu** — top-level entry under Play menu with Enabled toggle
- **CV rec arm destinations** — MTX_V0_REC_ARM and MTX_V1_REC_ARM in the CV matrix
- **Transport bar bleed fix** — bars no longer bleed through into submenus

## Recording operation

1. **Enable recording** — Play menu → Recording → Enabled: ON
2. **Arm a voice** — short-press the encoder on the main screen to cycle arm state (off → V0 armed → V1 armed → both)
   - Armed voice shows yellow transport bar
3. **Record** — send a trigger to TRIG0; armed voice(s) start recording from line-in
   - Bar turns red while recording
4. **Stop** — send another trigger to TRIG0 (or press encoder); file saves to SD
   - Bar turns green briefly, then file auto-loads into that slot
5. **CV arm** — assign MTX_V0_REC_ARM or MTX_V1_REC_ARM in the CV matrix to arm via CV gate instead of encoder

Files are saved as WAV to the SD card root. The auto-loaded file is immediately playable.
