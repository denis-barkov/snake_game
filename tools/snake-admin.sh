#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SNAKECLI_BIN=""

if [ -x /usr/local/bin/snakecli ]; then
  SNAKECLI_BIN="/usr/local/bin/snakecli"
elif [ -x "${ROOT_DIR}/tools/snakecli.py" ]; then
  SNAKECLI_BIN="python3 ${ROOT_DIR}/tools/snakecli.py"
else
  echo "snakecli not found"
  exit 1
fi

case "${1:-}" in
  seed|reset|reload|seed-reload|reset-seed|reset-seed-reload)
    # shellcheck disable=SC2086
    exec ${SNAKECLI_BIN} app "$1"
    ;;
  *)
    echo "Usage: snake-admin {seed|reset|reload|seed-reload|reset-seed|reset-seed-reload}"
    exit 1
    ;;
esac
