# shadow-fb-v1a — 2026-07-16

shadow-fb-v1 + the web SCREEN card (Remote tab: live TFT screenshot, Refresh
+ 3s auto) and ~0.8s captures (sparse row yields). The running firmware at
EOD 2026-07-16: preset_store/Keys patches + MP3 broadcast (out+in taps) +
shadow framebuffer. `./flash.sh [PORT]` = serial recovery, OTA layout.
NOTE: the TFT driver change lives in the ESP32_TFT_library SUBMODULE —
tungusk fork, branch shadow-framebuffer (see .gitmodules).
