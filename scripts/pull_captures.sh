#!/usr/bin/env bash
set -euo pipefail
HOST="${PYLONMON_HOST:-battery.local}"
DIR="$(cd "$(dirname "$0")/.." && pwd)/test/fixtures/raw"
DELETE=0; [ "${1:-}" = "--delete" ] && DELETE=1
mkdir -p "$DIR"
list=$(curl -sf --max-time 15 "http://$HOST/dbg/files")
echo "$list" | jq -r '"device: \(.count) files, used \(.used)/\(.total) bytes"'
new=0
for n in $(echo "$list" | jq -r '.files[].n'); do
  if [ ! -s "$DIR/$n" ]; then
    curl -sf --max-time 15 "http://$HOST/dbg/file?n=$n" -o "$DIR/$n.part" && mv "$DIR/$n.part" "$DIR/$n" && new=$((new+1))
  fi
  if [ $DELETE = 1 ] && [ -s "$DIR/$n" ]; then
    curl -sf --max-time 10 -X DELETE "http://$HOST/dbg/file?n=$n" >/dev/null
  fi
done
echo "downloaded $new new file(s) to $DIR"
