#!/bin/bash
# Soft-reboot the module over WiFi and wait for it to come back.
# Usage: tools/reboot.sh [IP]
# Refused while recording (the handler answers 409) — a reboot destroys an
# unsaved PSRAM take. Use this to clear the internal-RAM fragmentation that
# makes esp_ota_begin fail after a long flash session.
set -e
IP="${1:-192.168.3.227}"
RESP=$(curl -s -m 8 -X POST "http://$IP/reboot" || true)
case "$RESP" in
  *'"ok":true'*) echo "rebooting..." ;;
  *) echo "ABORT: $RESP"; exit 1 ;;
esac
for i in $(seq 1 20); do
  sleep 3
  s=$(curl -s -m 4 "http://$IP/ota/state" 2>/dev/null || true)
  [ -n "$s" ] && { echo "back: $s"; exit 0; }
done
echo "not back yet — check http://$IP/ota/state"; exit 1
