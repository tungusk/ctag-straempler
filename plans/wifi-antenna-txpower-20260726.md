# WiFi antenna band + TX power retest (2026-07-26)

Status: **BLOCKED on parts arrival.** Arlo realised while sourcing completion
parts that the antenna on unit 1 may be an **868/915 MHz LoRa whip, not a
2.4 GHz WiFi one**. If so, it may be part of why the module boot-loops with
the antenna attached — the bug we "fixed" by capping PHY TX power at 10 dBm.

This is worth settling before the 5-unit firmware is finalised, because the
fix currently in tree costs a factor of ten in radiated power on every unit.

## What we already know

Boot loop diagnosed and fixed 2026-07-02 (see the "WiFi antenna boot loop"
notes): the RF calibration burst inside `esp_wifi_start()` fires at the PHY
maximum and ignores `esp_wifi_set_max_tx_power()`, which only applies after
start. With the antenna attached the burst dips the supply rail and the chip
takes a POWERON_RESET (with "flash read err, 1000" noise from the ULP
toggling GPIO12/MTDI mid-reset). Bare, it boots fine — hence the historical
"attach the antenna after boot" workaround.

Shipped fix, in `sdkconfig.defaults:508-509` and `sdkconfig:582-583`:

```
CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER=10
CONFIG_ESP32_PHY_MAX_TX_POWER=10
```

These bound the calibration burst too, which `esp_wifi_set_max_tx_power()`
cannot. Verified at the time: boots with antenna attached, 0 resets, connects
in <2 s.

## Why the antenna is a suspect

At 2.4 GHz an 868 MHz whip is a badly mismatched load. A PA driving an
arbitrary mismatch can draw **more** current than into either a correct 50 Ω
load or an open connector, depending on the phase of the reflection
(load-pull). So "boots bare, boot-loops with antenna attached" is consistent
with a wrong antenna — it is not a contradiction, and it does not require the
antenna to be good for the crash to be real.

## The evidence against it being the whole cause

We verified in July that post-start TX at full power **with that same
antenna** ran three minutes of full-power associations with zero resets.
Sustained transmit into a bad match should stress the rail at least as hard
as a one-shot calibration burst. It didn't.

That points back at *when* the burst happens — boot, while flash, PSRAM, the
ULP, the display and the SD card are all inrushing — rather than what it
drives into. Best current read: the wrong antenna probably ate margin rather
than being the sole cause. Both can be true at once.

## Test plan

Arlo will have a spare from the arriving 5-set, so this runs on **unit 1**,
the existing prototype. That is the better experiment anyway: same board,
same firmware, only the antenna changes.

**Step 0 — confirm the band.** The Ali order (2026-07-26) includes an MGCKTD
antenna + u.FL pigtail 5-set and three straight mini RP-SMA stubs. Check the
listings/markings say 2.4 GHz before trusting them — the LoRa/WiFi mix-up is
exactly the one being investigated and the parts look identical.

**Step 1 — no-flash range check.** Swap antennas on the current 10 dBm
firmware. This proves *nothing* about the crash (it already boots fine at
10 dBm with the LoRa whip), but it does show range. A LoRa whip at 2.4 GHz
should be clearly worse, so a visible reliability-at-distance improvement is
independent confirmation the old part is wrong, with no flash spent.

**Step 2 — one test build.** Set both PHY values back to 20 **and** add RSSI
to `/status` in the *same* image, so one flash answers the boot question and
provides the instrument to measure the result. Cold boot with the antenna
attached. Three outcomes:

| Result | Conclusion | Action |
|---|---|---|
| Boots clean | Antenna was the cause | Keep 20 dBm on all 5 units |
| Still boot-loops | Boot-time rail margin is the cause | Restore the 10 dBm cap; stop blaming the antenna |
| Boots only with the 25 s deferred WiFi start | Inrush overlap | The defer in `wifi.c` is load-bearing, not vestigial — shorten it to probe, don't remove it blind |

## Side item: no RSSI anywhere in the firmware

Grepped 2026-07-26 — the tree reports RSSI nowhere, so there is no way to
compare two antennas' link quality from `/status`. Adding
`esp_wifi_sta_get_ap_info()` to the status JSON is a few lines. Fold it into
the step-2 build rather than spending a separate flash on it later.

## Notes for whoever runs this

- Announce before flashing; OTA reboots the device.
- Check `/status` `recording` before any flash — a reboot destroys an unsaved
  PSRAM take.
- `tools/ota.sh [IP]` is the flash path; `POST /reboot` for a clean restart.
- Runtime `txpwr` (CONFIG.JSN / `POST /settings`, 8..84 quarter-dBm) can lower
  power below the PHY cap but can never exceed it — so it is not a substitute
  for the sdkconfig change in step 2.
