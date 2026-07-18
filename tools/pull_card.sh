#!/usr/bin/env bash
# pull_card.sh — back up the module's sample POOL over the REST API. No flash, no
# pulling the SD card: every sample /files lists is fetched via /files/raw into a
# dated folder, with the real container extension (sniffed from the file header)
# and its .JSN sidecar. Resumable — a file already present at the same byte size
# is skipped, so re-running only grabs what's new/changed.
#
# Usage: tools/pull_card.sh [IP] [DEST_DIR]
#   tools/pull_card.sh                     # 192.168.3.227 -> ~/ctag-straempler-backups/card-<ts>
#   tools/pull_card.sh 192.168.1.50 ./bak  # custom host + dir
#
# NOTE: only the sample pool is exposed by /files (root CONFIG.JSN / AUTOSAVE.JSN
# are not). Pool files can be large (tens of MB) over WiFi — Ctrl-C is safe, the
# next run resumes.
set -euo pipefail

IP="${1:-192.168.3.227}"
DEST="${2:-$HOME/ctag-straempler-backups/card-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$DEST"
echo "Pool backup: http://$IP  ->  $DEST"

json=$(curl -s -m 20 "http://$IP/files") || { echo "ERROR: unreachable http://$IP/files"; exit 1; }
[ -n "$json" ] || { echo "ERROR: empty /files response"; exit 1; }

total=0; got=0; skip=0; fail=0
while IFS=$'\t' read -r name size; do
  [ -n "$name" ] || continue
  total=$((total+1))
  # already backed up at this exact size? skip (makes re-runs resumable)
  existing=$(ls -1 "$DEST/$name".* 2>/dev/null | grep -v '\.JSN$' | head -1 || true)
  if [ -n "$existing" ] && [ "$(wc -c < "$existing" | tr -d ' ')" = "$size" ]; then
    echo "  = $name (have, $size B)"; skip=$((skip+1)); continue
  fi
  tmp="$DEST/.$name.part"
  if ! curl -s -f -m 600 "http://$IP/files/raw?name=$name" -o "$tmp"; then
    echo "  x $name (download failed)"; rm -f "$tmp"; fail=$((fail+1)); continue
  fi
  # sniff the container from the first 4 bytes -> real extension
  magic=$(head -c4 "$tmp" 2>/dev/null || true)
  case "$magic" in
    RIFF) ext=wav ;;
    FORM) ext=aif ;;
    *)    ext=raw ;;
  esac
  mv -f "$tmp" "$DEST/$name.$ext"
  # the .JSN sidecar carries bpm/slices — grab it if present (optional)
  curl -s -f -m 60 "http://$IP/files/raw?name=$name.JSN" -o "$DEST/$name.JSN" 2>/dev/null || rm -f "$DEST/$name.JSN"
  echo "  + $name.$ext ($(wc -c < "$DEST/$name.$ext" | tr -d ' ') B)"
  got=$((got+1))
done < <(printf '%s' "$json" | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception as e:
    sys.stderr.write("JSON parse error: %s\n" % e); sys.exit(1)
for f in d.get("files", []):
    print("%s\t%s" % (f.get("name", ""), f.get("size", 0)))
')

echo "Done: $got fetched, $skip already had, $fail failed (of $total) -> $DEST"
