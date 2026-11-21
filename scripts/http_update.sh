#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ENV="${1:-probe}"
HOST="${PYLONMON_HOST:-battery.local}"
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
"$PIO" run -e "$ENV" >/dev/null
BIN=".pio/build/$ENV/firmware.bin"
PASS="$(sed -n 's/^#define OTA_PASS *"\(.*\)".*/\1/p' include/secrets.h)"
echo "uploading $BIN ($(stat -c %s "$BIN") bytes) to $HOST"
for path in /api/v1/update /dbg/update; do
  code=$(curl -sS --max-time 240 -o /dev/null -w "%{http_code}" \
    -H "Origin: http://$HOST" -u "admin:$PASS" -F "update=@$BIN" "http://$HOST$path")
  echo "$path -> http $code"
  [ "$code" = 404 ] || break
done
[ "$code" = 200 ] || { echo "update failed"; exit 1; }
for i in $(seq 1 30); do
  st=$(curl -s -m 4 "http://$HOST/api/v1/status" || true)
  if echo "$st" | jq -e '.fw' >/dev/null 2>&1; then echo "$st" | jq -c '{fw, uptime_s, reset_reason, wifi: .wifi.mode}'; exit 0; fi
  sleep 2
done
echo "device did not come back within 60 s"; exit 1
