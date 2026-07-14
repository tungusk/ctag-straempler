#!/bin/bash
# Flash doubledecker-v1 firmware to ESP32 Strampler

PORT="${1:-/dev/cu.usbserial-3110}"
DIR="$(cd "$(dirname "$0")" && pwd)"

ESPTOOL=~/.espressif/python_env/idf4.3_py3.9_env/bin/python
IDF_ESPTOOL=~/esp/esp-idf-v4.3/components/esptool_py/esptool/esptool.py

echo "Flashing doubledecker-v1 to $PORT..."

$ESPTOOL $IDF_ESPTOOL \
  -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset --chip esp32 \
  write_flash --flash_mode dio --flash_size detect --flash_freq 80m \
  0x1000  "$DIR/bootloader.bin" \
  0x8000  "$DIR/partition-table.bin" \
  0x10000 "$DIR/ctag-straempler.bin"
