#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MONITOR_PID=""

cleanup() {
  if [[ -n "${MONITOR_PID}" ]]; then
    kill "${MONITOR_PID}" 2>/dev/null || true
  fi
}

trap cleanup EXIT INT TERM

node "${ROOT_DIR}/scripts/spot-relay-live.mjs" --json-out "${ROOT_DIR}/data/spot-relay-live.json" &
MONITOR_PID=$!

npm --prefix "${ROOT_DIR}/dashboard" run dev
