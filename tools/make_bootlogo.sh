#!/bin/bash
# make_bootlogo.sh — turn (almost) any image into a Strämpler-compatible
# bootlogo.bmp: 320x240, 24-bit, uncompressed, classic 54-byte BMP header.
#
# The firmware's BMP loader (TFT_bmp_image) silently rejects anything else —
# modern exporters write V4/V5 headers / 32-bit / top-down rows and you get
# a blank screen. This script letterboxes onto a black 320x240 canvas (no
# stretching) and rewrites the container to the exact legacy layout.
#
# Usage: tools/make_bootlogo.sh input.(png|jpg|bmp|heic|...) [output.bmp]
#        then copy the output to the SD card root as bootlogo.bmp
set -e
IN="$1"
OUT="${2:-bootlogo.bmp}"
[ -f "$IN" ] || { echo "usage: $0 input-image [output.bmp]"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1) decode anything -> BMP, resized to FIT inside 320x240 (aspect preserved)
W=$(sips -g pixelWidth  "$IN" | awk '/pixelWidth/{print $2}')
H=$(sips -g pixelHeight "$IN" | awk '/pixelHeight/{print $2}')
if [ $((W * 240)) -ge $((H * 320)) ]; then
    sips -s format bmp --resampleWidth 320 "$IN" --out "$TMP/in.bmp" >/dev/null
else
    sips -s format bmp --resampleHeight 240 "$IN" --out "$TMP/in.bmp" >/dev/null
fi

# 2) normalize container: any BMP variant -> 54-byte-header 24-bit bottom-up
python3 - "$TMP/in.bmp" "$OUT" <<'EOF'
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
assert d[:2] == b'BM', 'sips did not produce a BMP'
off  = struct.unpack('<I', d[10:14])[0]
hsz  = struct.unpack('<I', d[14:18])[0]
w    = struct.unpack('<i', d[18:22])[0]
h    = struct.unpack('<i', d[22:26])[0]
bpp  = struct.unpack('<H', d[28:30])[0]
comp = struct.unpack('<I', d[30:34])[0]
assert bpp in (24, 32), f'unsupported bit depth {bpp}'
assert comp in (0, 3), f'compressed BMP not supported (compression={comp})'
topdown = h < 0
h = abs(h)
bypp = bpp // 8
srow = (w * bypp + 3) & ~3
# read source pixels into row-major top-down list of (b,g,r)
rows = []
for y in range(h):
    sy = y if topdown else (h - 1 - y)
    base = off + sy * srow
    row = [d[base+x*bypp : base+x*bypp+3] for x in range(w)]
    rows.append(row)
# compose centered on 320x240 black
CW, CH = 320, 240
canvas = [[b'\x00\x00\x00'] * CW for _ in range(CH)]
x0, y0 = (CW - w) // 2, (CH - h) // 2
for y in range(h):
    if 0 <= y0 + y < CH:
        for x in range(w):
            if 0 <= x0 + x < CW:
                canvas[y0 + y][x0 + x] = rows[y][x]
# write minimal legacy BMP: 14-byte file header + 40-byte info header
drow = CW * 3                       # 960, already 4-byte aligned
img  = drow * CH
out  = bytearray()
out += b'BM' + struct.pack('<IHHI', 54 + img, 0, 0, 54)
out += struct.pack('<IiiHHIIiiII', 40, CW, CH, 1, 24, 0, img, 3780, 3780, 0, 0)
for y in range(CH - 1, -1, -1):     # bottom-up
    out += b''.join(canvas[y])
open(dst, 'wb').write(out)
print(f'{dst}: 320x240x24 legacy BMP, {len(out)} bytes (source {w}x{h}@{bpp})')
EOF

file "$OUT"