#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly base_url="${SCRY_LOCAL_MODEL_BASE_URL:-}"
readonly health_url="${SCRY_LOCAL_MODEL_HEALTH_URL:-}"
readonly model="${SCRY_LOCAL_MODEL_MODEL:-}"
readonly health_timeout="${SCRY_LOCAL_MODEL_HEALTH_TIMEOUT_SECONDS:-60}"
readonly turn_timeout="${SCRY_LOCAL_MODEL_TIMEOUT_SECONDS:-180}"
readonly artifact_dir="${root_dir}/build/nightly-local-model-artifacts"
readonly log_file="${artifact_dir}/local-model-smoke.log"

require_environment() {
  local name="$1"
  local value="$2"
  if [[ -z "${value}" ]]; then
    echo "Required environment variable is unset: ${name}" >&2
    exit 2
  fi
}

require_positive_integer() {
  local name="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "${name} must be a positive integer, got: ${value}" >&2
    exit 2
  fi
}

run_with_timeout() {
  local seconds="$1"
  shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "${seconds}" "$@"
    return
  fi
  if command -v gtimeout >/dev/null 2>&1; then
    gtimeout "${seconds}" "$@"
    return
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 -c '
import subprocess
import sys

try:
    result = subprocess.run(sys.argv[2:], timeout=int(sys.argv[1]), check=False)
except subprocess.TimeoutExpired:
    raise SystemExit(124)
raise SystemExit(result.returncode)
' "${seconds}" "$@"
    return
  fi
  echo "A timeout command or Python 3 is required." >&2
  exit 2
}

wait_for_health() {
  local deadline=$((SECONDS + health_timeout))
  local response=""

  while ((SECONDS < deadline)); do
    if response="$(curl \
      --silent \
      --show-error \
      --fail \
      --max-time 2 \
      "${health_url}" 2>>"${log_file}")"; then
      echo "Local model server is healthy: ${response}" | tee -a "${log_file}"
      return
    fi
    sleep 1
  done

  echo "Local model health check timed out after ${health_timeout}s." |
    tee -a "${log_file}" >&2
  exit 1
}

require_environment SCRY_LOCAL_MODEL_BASE_URL "${base_url}"
require_environment SCRY_LOCAL_MODEL_HEALTH_URL "${health_url}"
require_environment SCRY_LOCAL_MODEL_MODEL "${model}"
require_positive_integer \
  SCRY_LOCAL_MODEL_HEALTH_TIMEOUT_SECONDS \
  "${health_timeout}"
require_positive_integer SCRY_LOCAL_MODEL_TIMEOUT_SECONDS "${turn_timeout}"

command -v curl >/dev/null 2>&1 || {
  echo "Required command is unavailable: curl" >&2
  exit 2
}
command -v cmake >/dev/null 2>&1 || {
  echo "Required command is unavailable: cmake" >&2
  exit 2
}

mkdir -p "${artifact_dir}"
: >"${log_file}"
wait_for_health

cd "${root_dir}"
cmake --preset nightly-local-model
cmake --build build/nightly-local-model --target scry_local_model_smoke

echo "Running public-API local-model smoke against ${model}." |
  tee -a "${log_file}"
run_with_timeout "${turn_timeout}" \
  build/nightly-local-model/scry_local_model_smoke \
  2>&1 | tee -a "${log_file}"
