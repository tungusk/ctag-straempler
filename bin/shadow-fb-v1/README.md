# shadow-fb-v1 — 2026-07-16

PSRAM shadow framebuffer: every TFT draw writes through to a 320x240 PSRAM
copy, and `/screenshot` (24-bit BMP) serves it — remote eye-testing works on
this unit despite the dead GRAM readback. Includes the Keys ENV-overlap fix
it immediately caught. `./flash.sh [PORT]` = serial recovery, OTA layout.
