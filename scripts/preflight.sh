#!/usr/bin/env bash

# Local equivalent of the per-commit CI ring: documentation, core, profiling
# infrastructure, clang-tidy, sanitizers, and the GCC 16 reflection component.
# Fuzzing, the showcase, and the local-model smoke live in the
# scheduled/manual nightly workflow.

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

run_reflection() {
  if ! command -v g++-16 >/dev/null 2>&1; then
    echo "g++-16 is unavailable; the hosted Linux reflection leg is authoritative" >&2
    return 1
  fi
  ./scripts/ci-reflection.sh
}

run_profile_smoke() {
  local artifact_dir=""
  mkdir -p "${root_dir}/build/profile-artifacts"
  artifact_dir="$(mktemp -d "${root_dir}/build/profile-artifacts/preflight.XXXXXX")" ||
    return 1
  ./scripts/perf-run.sh \
    --mode smoke \
    --output "${artifact_dir}/all-scenarios" &&
    python3 scripts/perf-compare.py compare \
      --base "${artifact_dir}/all-scenarios" \
      --head "${artifact_dir}/all-scenarios" \
      --allow-dirty \
      --output-dir "${artifact_dir}/all-scenarios/self-comparison" &&
    ./scripts/perf-pair.sh \
      --base-dir "${root_dir}" \
      --head-dir "${root_dir}" \
      --output "${artifact_dir}/paired-smoke" \
      --mode smoke \
      --cycles 1 \
      --filter '^SSE/chunk_bytes:4096/crlf:0$' \
      --allow-dirty \
      --informational-reason "local preflight paired smoke"
}

cd "${root_dir}"
run_gate "Doxygen API site" ./scripts/ci-docs.sh
run_gate "core" ./scripts/ci-local.sh
run_gate "profiling semantic + paired smoke" run_profile_smoke
run_gate "clang-tidy" run_tidy
run_gate "ASan + UBSan" run_preset asan
# TSan is where nondeterminism surfaces; the repeat runs live here (QA-008).
run_gate "TSan" run_preset tsan --repeat until-fail:3
run_gate "GCC 16 supported reflection component" run_reflection

if [[ "${failures}" -ne 0 ]]; then
  echo
  echo "Preflight failed in ${failures} gate(s)." >&2
  exit 1
fi

echo
echo "Preflight passed."
