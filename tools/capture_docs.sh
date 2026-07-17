#!/bin/bash
# Refresh docs/machines/img/ screenshots from the live device (shadow FB).
# Usage: tools/capture_docs.sh [IP]      (macOS: uses sips for BMP->PNG)
# Switches through every machine, grabs its boot page. Radio gets a station
# playing first so the shot shows a live stream. Leaves the module on Deck.
set -e
IP="${1:-192.168.3.227}"
DIR="$(cd "$(dirname "$0")/.." && pwd)/docs/machines/img"
mkdir -p "$DIR"

shot() {  # shot <basename>
  curl -s -m 20 "http://$IP/screenshot" -o /tmp/cap_$$.bmp
  sips -s format png /tmp/cap_$$.bmp --out "$DIR/$1.png" >/dev/null 2>&1
  rm -f /tmp/cap_$$.bmp
  echo "  $1.png"
}
sw() { curl -s -m 8 -X POST "http://$IP/remote/machine?name=$1" >/dev/null; }

curl -s -m 5 "http://$IP/status" >/dev/null || { echo "device not answering"; exit 1; }

for M in Sampler Looper Slicer Granular Glitch Drums Deck DoubleDecker \
         Tracker Freesound Synth Keys Tape Editor; do
  echo "$M..."
  sw "$M"
  case "$M" in Slicer|Tracker|Deck|DoubleDecker) sleep 25 ;; *) sleep 4 ;; esac  # scans/loads
  case "$M" in
    DoubleDecker) shot dualdeck-live ;;
    *)            shot "$(echo "$M" | tr '[:upper:]' '[:lower:]')-live" ;;
  esac
done

echo "Radio (playing)..."
sw Radio; sleep 3
curl -s -m 8 -X POST "http://$IP/radio/play?station=0" >/dev/null; sleep 8
shot radio-live
curl -s -m 8 -X POST "http://$IP/radio/stop" >/dev/null

sw Deck
echo "done -> $DIR (module parked on Deck)"
