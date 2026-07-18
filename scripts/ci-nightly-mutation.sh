#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/nightly-mutation"
readonly report_root="${root_dir}/build/nightly-mutation-reports"
readonly plugin_path="${MULL_IR_FRONTEND:-/usr/lib/mull-ir-frontend-18}"
readonly runner="${MULL_RUNNER:-mull-runner-18}"
readonly workers="${SCRY_MULL_WORKERS:-2}"
readonly mutant_timeout_ms="${SCRY_MULL_TIMEOUT_MS:-30000}"

require_command() {
  local command_name="$1"
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "Required command is unavailable: ${command_name}" >&2
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

run_mutation_target() {
  local name="$1"
  local binary="$2"
  local report_dir="${report_root}/${name}"
  local log_file="${report_dir}/mull.log"

  if [[ ! -x "${binary}" ]]; then
    echo "Mutation target is missing or not executable: ${binary}" >&2
    exit 1
  fi

  mkdir -p "${report_dir}"
  "${runner}" \
    --strict \
    --allow-surviving \
    --workers "${workers}" \
    --timeout "${mutant_timeout_ms}" \
    --reporters IDE \
    --reporters Elements \
    --reporters Sarif \
    --report-dir "${report_dir}" \
    --no-mutant-output \
    "${binary}" \
    2>&1 | tee "${log_file}"

  if ! grep -Fq "Mutation score:" "${log_file}"; then
    echo "Mull did not produce a mutation score for ${name}." >&2
    exit 1
  fi
  if [[ -z "$(find "${report_dir}" -type f ! -name mull.log -print -quit)" ]]; then
    echo "Mull did not produce structured reports for ${name}." >&2
    exit 1
  fi
}

require_command cmake
require_command clang-18
require_command clang++-18
require_command "${runner}"
require_positive_integer SCRY_MULL_WORKERS "${workers}"
require_positive_integer SCRY_MULL_TIMEOUT_MS "${mutant_timeout_ms}"

if [[ ! -f "${plugin_path}" ]]; then
  echo "Mull LLVM 18 frontend is unavailable: ${plugin_path}" >&2
  echo "Install Mull 0.34.0's LLVM 18 Ubuntu 24.04 package before running." >&2
  exit 2
fi
if ! "${runner}" --version | grep -Fq "0.34.0"; then
  echo "Mull runner must be pinned to version 0.34.0." >&2
  exit 2
fi

export MULL_CONFIG="${root_dir}/mull.yml"
cmake -E remove_directory "${build_dir}"
cmake -E remove_directory "${report_root}"

CC=clang-18 CXX=clang++-18 cmake \
  -S "${root_dir}" \
  -B "${build_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_CXX_FLAGS=-fpass-plugin=${plugin_path} -g -grecord-command-line" \
  -DSCRY_BUILD_EXAMPLES=OFF \
  -DSCRY_BUILD_TESTS=ON \
  -DSCRY_ENABLE_FORMAT_CHECK=OFF \
  -DSCRY_USE_LIBCXX=ON \
  -DSCRY_WARNINGS_AS_ERRORS=ON
cmake --build "${build_dir}" --target \
  scry_turn_machine_tests \
  scry_sse_tests \
  scry_retry_tests

run_mutation_target \
  machine \
  "${build_dir}/tests/machine/scry_turn_machine_tests"
run_mutation_target \
  sse \
  "${build_dir}/tests/protocol/scry_sse_tests"
run_mutation_target \
  retry \
  "${build_dir}/scry_retry_tests"

echo "Mull reports written to ${report_root}; surviving mutants are reported."
