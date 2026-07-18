#!/usr/bin/env bash

set -uo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
failures=0

run_gate() {
  local name="$1"
  shift
  echo
  echo "==> ${name}"
  if "$@"; then
    echo "PASS: ${name}"
  else
    echo "FAIL: ${name}" >&2
    failures=$((failures + 1))
  fi
}

run_tidy() {
  local tidy_path="${PATH}"
  if ! command -v clang-tidy >/dev/null 2>&1 &&
    command -v brew >/dev/null 2>&1 &&
    brew list --versions llvm >/dev/null 2>&1; then
    tidy_path="$(brew --prefix llvm)/bin:${tidy_path}"
  fi
  if ! PATH="${tidy_path}" command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy is unavailable" >&2
    return 1
  fi
  PATH="${tidy_path}" cmake \
    --preset ci \
    -B build/tidy \
    -DSCRY_ENABLE_CLANG_TIDY=ON \
    -DSCRY_ENABLE_FORMAT_CHECK=OFF &&
    PATH="${tidy_path}" cmake --build build/tidy
}

run_preset() {
  local preset="$1"
  cmake --preset "${preset}" &&
    cmake --build "build/${preset}" &&
    ctest \
      --test-dir "build/${preset}" \
      --output-on-failure \
      --repeat until-fail:3
}

run_reflection() {
  if ! command -v g++-16 >/dev/null 2>&1; then
    echo "g++-16 is unavailable; the hosted Linux reflection leg is authoritative" >&2
    return 1
  fi
  cmake --preset reflection-gcc16 &&
    cmake --build build/reflection-gcc16
}

cd "${root_dir}"
run_gate "core" ./scripts/ci-local.sh
run_gate "quality ratchet" ./scripts/quality-gate.sh
run_gate "clang-tidy" run_tidy
run_gate "ASan + UBSan" run_preset asan
run_gate "TSan" run_preset tsan
run_gate "libcurl feasibility" run_preset curl
run_gate "GCC 16 reflection feasibility" run_reflection

if [[ "${failures}" -ne 0 ]]; then
  echo
  echo "Preflight failed in ${failures} gate(s)." >&2
  exit 1
fi

echo
echo "Preflight passed."
