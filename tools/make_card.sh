#!/bin/bash
# Assemble the starter card: card/ + bootlogo + a silence file -> build/starter-card.zip
set -e
cd "$(dirname "$0")/.."
OUT=build/starter-card
rm -rf "$OUT" && mkdir -p "$OUT"
cp card/CONFIG.JSN card/README.md bootlogo/bootlogo.bmp "$OUT/"
dd if=/dev/zero of="$OUT/SILENCE.RAW" bs=4 count=32768 status=none    # 8192*4 uint32 zeros, as fileio.c writes it
rm -f build/starter-card.zip
(cd "$OUT" && zip -q -X ../starter-card.zip CONFIG.JSN SILENCE.RAW bootlogo.bmp README.md)
ls -l build/starter-card.zip
