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
  shift
  cmake --preset "${preset}" &&
    cmake --build "build/${preset}" &&
    ctest \
      --test-dir "build/${preset}" \
      --output-on-failure \
      "$@"
}

run_fuzz() {
  local build_dir="build/fuzz"
  if [[ "$(uname -s)" == "Darwin" ]] &&
    command -v brew >/dev/null 2>&1 &&
    brew list --versions llvm >/dev/null 2>&1; then
    local llvm_bin
    llvm_bin="$(brew --prefix llvm)/bin"
    build_dir="build/fuzz-llvm"
    CC="${llvm_bin}/clang" CXX="${llvm_bin}/clang++" \
      cmake --preset fuzz -B "${build_dir}" || return
  else
    cmake --preset fuzz || return
  fi
  cmake --build "${build_dir}" \
    --target scry_sse_fuzz scry_anthropic_fuzz scry_openai_fuzz &&
    ctest \
      --test-dir "${build_dir}" \
      --output-on-failure \
      --repeat until-fail:3 \
      -R fuzz
}

run_reflection() {
  if ! command -v g++-16 >/dev/null 2>&1; then
    echo "g++-16 is unavailable; the hosted Linux reflection leg is authoritative" >&2
    return 1
  fi
  ./scripts/ci-reflection.sh
}

cd "${root_dir}"
run_gate "core (with libcurl runtime)" ./scripts/ci-local.sh -DSCRY_BUILD_CURL_SPIKE=ON
run_gate "quality gate" ./scripts/quality-gate.sh
run_gate "clang-tidy" run_tidy
run_gate "ASan + UBSan" run_preset asan
# TSan is where nondeterminism surfaces; the repeat runs live here (QA-008).
run_gate "TSan" run_preset tsan --repeat until-fail:3
run_gate "short protocol fuzzing" run_fuzz
run_gate "opt-in showcase" ./scripts/ci-showcase.sh
run_gate "GCC 16 supported reflection component" run_reflection

if [[ "${failures}" -ne 0 ]]; then
  echo
  echo "Preflight failed in ${failures} gate(s)." >&2
  exit 1
fi

echo
echo "Preflight passed."
