# Starter card

What goes on a blank SD card before the first boot. The firmware builds the
rest itself: `pool/ raw/ banks/ usr/` and the machine folders appear on the
first boot, `usr/REC`, `usr/LOOPS` and the preset folders on first use.

**Card format: FAT32 with an MBR partition table.** Cards over 32 GB ship as
exFAT, which the firmware cannot mount, and it will not format a card by
itself. On a Mac:

    diskutil eraseDisk FAT32 STRAMP MBRFormat /dev/diskN

Then copy the contents of `starter-card.zip` (built by `tools/make_card.sh`)
to the root of the card:

| File | Why |
|---|---|
| `CONFIG.JSN` | Settings. WiFi and the Freesound key are placeholders: set them from the panel (Settings) or the web page once it is on the network. `encres 4, encdir 1` suit the current encoders; a first-prototype unit wants `2, 0`. `hostname` is what `.local` resolves to; give a second unit its own. |
| `SILENCE.RAW` | 128 KB of zeros the two legacy sample slots point at. |
| `bootlogo.bmp` | The boot screen. Optional; the firmware shows it only if present. |

A card with none of this also works: the firmware writes `CONFIG.JSN` and
`SILENCE.RAW` with the same placeholders, minus the encoder and hostname
settings, and boots without a logo.
