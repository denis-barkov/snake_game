#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_PATH=""
WORK_DIR=""

load_env_if_present() {
  if [ -f /etc/snake.env ]; then
    set -a
    # shellcheck disable=SC1091
    source /etc/snake.env
    set +a
  fi
}

resolve_binary() {
  if [ -x /opt/snake/snake_server ]; then
    BIN_PATH="/opt/snake/snake_server"
    WORK_DIR="/opt/snake"
    return 0
  fi

  if [ -x "${ROOT_DIR}/snake_server" ]; then
    BIN_PATH="${ROOT_DIR}/snake_server"
    WORK_DIR="${ROOT_DIR}"
    return 0
  fi

  if [ -x ./snake_server ]; then
    BIN_PATH="./snake_server"
    WORK_DIR="$(pwd)"
    return 0
  fi

  echo "snake_server binary not found. Expected /opt/snake/snake_server or ./snake_server"
  return 1
}

run_server_cmd() {
  local mode="$1"
  (cd "${WORK_DIR}" && "${BIN_PATH}" "${mode}")
}

reload_server() {
  if command -v systemctl >/dev/null 2>&1 && systemctl kill -s USR1 snake >/dev/null 2>&1; then
    return 0
  fi

  if pgrep -f "/snake_server serve" >/dev/null 2>&1; then
    pkill -USR1 -f "/snake_server serve"
    return 0
  fi

  if pgrep -x "snake_server" >/dev/null 2>&1; then
    pkill -USR1 -x "snake_server"
    return 0
  fi

  echo "No running snake_server process found for reload."
  return 1
}

load_env_if_present
resolve_binary

case "${1:-}" in
  seed)
    run_server_cmd seed
    ;;
  reset)
    run_server_cmd reset
    ;;
  reload)
    reload_server
    ;;
  seed-reload)
    run_server_cmd seed
    reload_server
    ;;
  reset-seed)
    run_server_cmd reset
    run_server_cmd seed
    ;;
  reset-seed-reload)
    run_server_cmd reset
    run_server_cmd seed
    reload_server
    ;;
  *)
    echo "Usage: snake-admin {seed|reset|reload|seed-reload|reset-seed|reset-seed-reload}"
    exit 1
    ;;
esac
