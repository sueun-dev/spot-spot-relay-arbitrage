#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${ROOT_DIR}/kimp_arb_cpp"
BUILD_DIR="${SRC_DIR}/build"
DEFAULT_CONFIG="${SRC_DIR}/config/config.yaml"
HELP_MODE=0
MONITOR_ONLY_MODE=0
HAS_CONFIG_ARG=0

for arg in "$@"; do
  if [[ "${arg}" == "-h" || "${arg}" == "--help" ]]; then
    HELP_MODE=1
  elif [[ "${arg}" == "--monitor-only" ]]; then
    MONITOR_ONLY_MODE=1
  elif [[ "${arg}" == "-c" || "${arg}" == "--config" || "${arg}" == --config=* ]]; then
    HAS_CONFIG_ARG=1
  fi
done

# Load .env automatically for exchange credentials.
if [[ -f "${ROOT_DIR}/.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env"
  set +a
fi

required_vars=(
  BITHUMB_API_KEY
  BITHUMB_SECRET_KEY
  BYBIT_API_KEY
  BYBIT_SECRET_KEY
)

missing_vars=()
for var_name in "${required_vars[@]}"; do
  if [[ -z "${!var_name:-}" ]]; then
    missing_vars+=("${var_name}")
  fi
done

if [[ ${HELP_MODE} -eq 0 ]]; then
  if [[ ${MONITOR_ONLY_MODE} -eq 0 && ${#missing_vars[@]} -gt 0 ]]; then
    echo "ERROR: Missing required environment variables: ${missing_vars[*]}" >&2
    echo "Check ${ROOT_DIR}/.env and fill the missing keys." >&2
    exit 1
  fi

  if command -v curl >/dev/null 2>&1; then
    if ! curl -sS --max-time 8 "https://api.bithumb.com/public/ticker/USDT_KRW" -o /dev/null; then
      echo "ERROR: Cannot reach Bithumb REST endpoint (https://api.bithumb.com)." >&2
      echo "Fix network/VPN/DNS first, then retry." >&2
      exit 1
    fi
    if ! curl -sS --max-time 8 "https://api.bybit.com/v5/market/tickers?category=linear" -o /dev/null; then
      echo "ERROR: Cannot reach Bybit REST endpoint (https://api.bybit.com)." >&2
      echo "Fix network/VPN/DNS first, then retry." >&2
      exit 1
    fi
  else
    echo "WARN: curl not found, skipping network preflight checks." >&2
  fi
fi

mkdir -p "${BUILD_DIR}"

if [[ ${HELP_MODE} -eq 0 || ! -x "${BUILD_DIR}/kimp_bot" ]]; then
  cmake -Wno-dev -S "${SRC_DIR}" -B "${BUILD_DIR}" >/dev/null
  cmake --build "${BUILD_DIR}" --target kimp_bot -j8 >/dev/null
fi

if [[ $# -eq 0 ]]; then
  set -- --config "${DEFAULT_CONFIG}"
elif [[ ${HAS_CONFIG_ARG} -eq 0 ]]; then
  set -- --config "${DEFAULT_CONFIG}" "$@"
fi

cd "${BUILD_DIR}"
exec env -u MallocStackLogging \
  -u MallocStackLoggingNoCompact \
  -u MallocStackLoggingDirectory \
  ./kimp_bot "$@"
