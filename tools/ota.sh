#!/bin/bash
# OTA-flash the current build over WiFi — no serial cable, ~14s, untethered.
# Usage: tools/ota.sh [IP]     (default 192.168.3.227)
# Requires the device to be running an OTA-capable image (synth-v1 migration+).
# Serial recovery if a pushed image ever fails to boot: bin/<archive>/flash.sh
set -e
IP="${1:-192.168.3.227}"
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/ctag-straempler.bin"
[ -f "$BIN" ] || { echo "no build at $BIN — run idf.py build first"; exit 1; }
echo "OTA -> http://$IP/ota  ($(du -h "$BIN" | cut -f1))"
BEFORE=$(curl -s -m 5 http://$IP/ota/state 2>/dev/null || true)
[ -n "$BEFORE" ] || { echo "ABORT: device not answering /ota/state (WiFi drop? radio playing?)"; exit 1; }
echo "before: $BEFORE"
RESP=$(curl -s -m 120 --data-binary @"$BIN" -H "Content-Type: application/octet-stream" "http://$IP/ota" || true)
echo "$RESP"
# the handler answers {"ok":true,...} on success; anything else (e.g. "oom"
# while radio plays — internal heap exhausted) means NOTHING was flashed
# "ota_begin failed" = internal RAM too fragmented for esp_ota_begin's buffer,
# the classic end-of-a-long-flash-session failure. A soft reboot reinitialises
# the heap and fixes it, so recover automatically instead of stalling on a
# power-cycle. Any OTHER rejection is not retried — "oom" while the radio plays
# needs the radio stopped, and retrying would just fail twice.
case "$RESP" in
  *'"ok":true'*) ;;
  *ota_begin*)
    echo "ota_begin failed (internal RAM fragmented) — soft-rebooting and retrying once"
    "$(dirname "$0")/reboot.sh" "$IP" || { echo "ABORT: reboot failed — power-cycle needed"; exit 1; }
    sleep 2
    RESP=$(curl -s -m 120 --data-binary @"$BIN" -H "Content-Type: application/octet-stream" "http://$IP/ota" || true)
    echo "$RESP"
    case "$RESP" in *'"ok":true'*) ;; *) echo "ABORT: still rejected after reboot — image NOT flashed"; exit 1;; esac ;;
  *) echo "ABORT: OTA rejected — image NOT flashed (stop radio and retry?)"; exit 1;; esac
echo "waiting for reboot into the new slot..."
for i in $(seq 1 15); do
  sleep 4
  s=$(curl -s -m 4 "http://$IP/ota/state" 2>/dev/null || true)
  [ -n "$s" ] && { echo "after:  $s"; exit 0; }
done
echo "device not back yet — check http://$IP/ota/state manually"
