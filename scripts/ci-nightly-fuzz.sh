#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly fuzz_kind="${1:-}"
readonly fuzz_seconds="${SCRY_NIGHTLY_FUZZ_SECONDS:-900}"
readonly per_input_timeout="${SCRY_NIGHTLY_FUZZ_INPUT_TIMEOUT_SECONDS:-10}"

usage() {
  echo "Usage: $0 {sse|anthropic|openai|response_policy|conversation}" >&2
}

require_positive_integer() {
  local name="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "${name} must be a positive integer, got: ${value}" >&2
    exit 2
  fi
}

if [[ "$#" -ne 1 ]]; then
  usage
  exit 2
fi

case "${fuzz_kind}" in
  sse | anthropic | openai | response_policy | conversation) ;;
  *)
    usage
    exit 2
    ;;
esac
readonly target="scry_${fuzz_kind}_fuzz"

require_positive_integer SCRY_NIGHTLY_FUZZ_SECONDS "${fuzz_seconds}"
require_positive_integer \
  SCRY_NIGHTLY_FUZZ_INPUT_TIMEOUT_SECONDS \
  "${per_input_timeout}"

readonly build_dir="${root_dir}/build/nightly-fuzz-${fuzz_kind}"
readonly artifact_dir="${root_dir}/build/nightly-fuzz-artifacts/${fuzz_kind}"
readonly runtime_corpus="${artifact_dir}/corpus"
readonly crash_dir="${artifact_dir}/crashes"
readonly seed_corpus="${root_dir}/tests/fuzz/corpus/${fuzz_kind}"
readonly log_file="${artifact_dir}/fuzz.log"

mkdir -p "${runtime_corpus}" "${crash_dir}"
cd "${root_dir}"

cmake --preset fuzz -B "${build_dir}"
cmake --build "${build_dir}" --target "${target}"

echo "Running ${target} for ${fuzz_seconds}s; artifacts: ${artifact_dir}"
timeout "$((fuzz_seconds + 120))" \
  "${build_dir}/tests/fuzz/${target}" \
  "-max_total_time=${fuzz_seconds}" \
  "-timeout=${per_input_timeout}" \
  -rss_limit_mb=4096 \
  -print_final_stats=1 \
  "-artifact_prefix=${crash_dir}/" \
  "${runtime_corpus}" \
  "${seed_corpus}" \
  2>&1 | tee "${log_file}"
