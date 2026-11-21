#!/usr/bin/env bash
#   scripts/cmd.sh pwr | scripts/cmd.sh "bat 1" | scripts/cmd.sh help --save
set -euo pipefail
HOST="${PYLONMON_HOST:-battery.local}"
CMD="${1:?usage: cmd.sh \"<command>\" [--save] [--hex]}"; shift
ARGS=()
for a in "$@"; do
  case "$a" in --save) ARGS+=(--data-urlencode save=1);; --hex) ARGS+=(--data-urlencode hex=1);; esac
done
curl -sS -i --max-time 20 -G "http://$HOST/dbg/cmd" --data-urlencode "c=$CMD" "${ARGS[@]}"
echo
