#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly fuzz_kind="${1:-}"
readonly fuzz_seconds="${SCRY_NIGHTLY_FUZZ_SECONDS:-900}"
readonly per_input_timeout="${SCRY_NIGHTLY_FUZZ_INPUT_TIMEOUT_SECONDS:-10}"

usage() {
  echo "Usage: $0 {sse|anthropic|openai}" >&2
}

require_positive_integer() {
  local name="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "${name} must be a positive integer, got: ${value}" >&2
    exit 2
  fi
}

timeout_command() {
  if command -v timeout >/dev/null 2>&1; then
    command -v timeout
    return
  fi
  if command -v gtimeout >/dev/null 2>&1; then
    command -v gtimeout
    return
  fi
  echo "GNU timeout is required (install coreutils on macOS)." >&2
  exit 2
}

if [[ "$#" -ne 1 ]]; then
  usage
  exit 2
fi

case "${fuzz_kind}" in
  sse)
    readonly target="scry_sse_fuzz"
    readonly binary_subdir="tests/protocol"
    ;;
  anthropic)
    readonly target="scry_anthropic_fuzz"
    readonly binary_subdir="tests/provider"
    ;;
  openai)
    readonly target="scry_openai_fuzz"
    readonly binary_subdir="tests/provider"
    ;;
  *)
    usage
    exit 2
    ;;
esac

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
readonly timeout_bin="$(timeout_command)"

if [[ ! -d "${seed_corpus}" ]]; then
  echo "Missing seed corpus: ${seed_corpus}" >&2
  if [[ "${fuzz_kind}" == "openai" ]]; then
    echo "Expected M4 wiring: tests/fuzz/corpus/openai plus" >&2
    echo "tests/provider/openai_fuzz.cpp and a SCRY_BUILD_FUZZERS-guarded" >&2
    echo "scry_openai_fuzz target in tests/provider/CMakeLists.txt." >&2
  fi
  exit 1
fi

mkdir -p "${runtime_corpus}" "${crash_dir}"
cd "${root_dir}"

cmake --preset fuzz -B "${build_dir}"
if ! cmake --build "${build_dir}" --target help | grep -Eq \
  "(^|[[:space:]])${target}([:[:space:]]|$)"; then
  echo "Configured build does not define ${target}." >&2
  echo "Expected root wiring: SCRY_BUILD_FUZZERS=ON and the provider/protocol" >&2
  echo "test subdirectory included from the root test build." >&2
  if [[ "${fuzz_kind}" == "openai" ]]; then
    echo "Expected provider wiring: tests/provider/openai_fuzz.cpp and a" >&2
    echo "SCRY_BUILD_FUZZERS-guarded target in tests/provider/CMakeLists.txt." >&2
  fi
  exit 1
fi
cmake --build "${build_dir}" --target "${target}"

readonly binary="${build_dir}/${binary_subdir}/${target}"
if [[ ! -x "${binary}" ]]; then
  echo "Built fuzz executable is missing or not executable: ${binary}" >&2
  exit 1
fi

echo "Running ${target} for ${fuzz_seconds}s; artifacts: ${artifact_dir}"
"${timeout_bin}" "$((fuzz_seconds + 120))" \
  "${binary}" \
  "-max_total_time=${fuzz_seconds}" \
  "-timeout=${per_input_timeout}" \
  -rss_limit_mb=4096 \
  -print_final_stats=1 \
  "-artifact_prefix=${crash_dir}/" \
  "${runtime_corpus}" \
  "${seed_corpus}" \
  2>&1 | tee "${log_file}"
