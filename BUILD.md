# Building and Flashing

## Prerequisites

ESP-IDF must be sourced before any `idf.py` commands:

```bash
source ~/esp/esp-idf/export.sh
```

## Build both branches

```bash
cd /Users/arlo/claude09/ctag-straempler

# --- overhaul (Phase 0–4, granular, CV scrubbing, env follower) ---
git checkout overhaul
idf.py build
cp build/ctag-straempler.bin bin/overhaul.bin

# --- master (stable recording baseline) ---
git checkout master
idf.py fullclean && idf.py build
cp build/ctag-straempler.bin bin/master.bin

git checkout overhaul   # return to working branch
```

`fullclean` is needed between branches to reset the CMake cache and sdkconfig.

## Flash

Connect the module via USB. The port is usually `/dev/tty.SLAB_USBtoUART` (CP2102).
If it doesn't appear, check: `ls /dev/tty.SLAB_USBtoUART /dev/tty.usbserial-*`

```bash
# Option A — use the existing flash script (from bin/)
cd bin/
./flash.sh
# Answer 'y' to pull fresh binaries from ../build, or 'n' to use what's already in bin/

# Option B — idf.py (auto-detects port, must have just run idf.py build)
idf.py -p /dev/tty.SLAB_USBtoUART flash

# Option C — flash a saved .bin directly with esptool
python $IDF_PATH/components/esptool_py/esptool/esptool.py \
  --chip esp32 --port /dev/tty.SLAB_USBtoUART -b 460800 \
  --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 80m --flash_size detect \
  0x1000  bin/bootloader.bin \
  0x8000  bin/partition-table.bin \
  0x10000 bin/overhaul.bin
```

## Monitor serial output

```bash
idf.py -p /dev/tty.SLAB_USBtoUART monitor
# Ctrl+] to exit
```
