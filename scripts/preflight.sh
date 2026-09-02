#!/usr/bin/env bash

# Local equivalent of the per-commit CI ring: documentation, core, clang-tidy,
# sanitizers, and the fuzz corpus replay. Long fuzz runs, the showcase, and the
# local-model smoke live in the scheduled/manual nightly workflow.
#
# A leg whose toolchain this host cannot provide is reported as SKIP rather than
# FAIL, and named again in the summary, so a reader can see exactly which hosted
# legs remain authoritative for the change.

set -uo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
failures=0
# Newline-separated so gate names containing spaces survive; bash 3.2 makes
# empty arrays under `set -u` more trouble than they are worth.
skipped_gates=""

run_gate() {
  local name="$1"
  shift
  echo
  echo "==> ${name}"
  local status=0
  "$@" || status=$?
  if [[ "${status}" -eq 0 ]]; then
    echo "PASS: ${name}"
  elif [[ "${status}" -eq 77 ]]; then
    # 77 is the conventional "skipped" status: this host cannot run the leg at
    # all, so hosted CI is authoritative for it.
    echo "SKIP: ${name} (unavailable on this host; hosted CI is authoritative)"
    skipped_gates="${skipped_gates}${name}"$'\n'
  else
    echo "FAIL: ${name}" >&2
    failures=$((failures + 1))
  fi
}

run_tidy() {
  local tidy_path="${PATH}"
  # CI pins clang-tidy 18, so a keg-only llvm@18 is preferred over whatever
  # major version the unversioned llvm formula currently points at.
  if ! command -v clang-tidy >/dev/null 2>&1 &&
    command -v brew >/dev/null 2>&1; then
    local formula=""
    for formula in llvm@18 llvm; do
      if brew list --versions "${formula}" >/dev/null 2>&1; then
        tidy_path="$(brew --prefix "${formula}")/bin:${tidy_path}"
        break
      fi
    done
  fi
  if ! PATH="${tidy_path}" command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy is unavailable" >&2
    return 1
  fi
  if ! PATH="${tidy_path}" command -v clang >/dev/null 2>&1 ||
    ! PATH="${tidy_path}" command -v clang++ >/dev/null 2>&1; then
    echo "Clang is unavailable; the clang-tidy leg requires its matching compiler" >&2
    return 1
  fi
  # SCRY_CLANG_TOOLING builds only the portable C++23 implementation, which is
  # the whole analyzable surface: the public API is C++26 and GCC-only. The
  # compiler is named explicitly because the ci preset pins g++-16.
  PATH="${tidy_path}" cmake \
    --preset ci \
    --fresh \
    -B build/tidy \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DSCRY_CLANG_TOOLING=ON \
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

# GCC ships no sanitizer runtime for some host/sanitizer combinations — on Apple
# Silicon the thread runtime is missing entirely — so each sanitizer leg probes
# its own flag and reports host-unavailable rather than failing a preflight.
gcc_sanitizer_links() {
  local flag="$1"
  local probe_dir=""
  probe_dir="$(mktemp -d)" || return 1
  printf 'int main() { return 0; }\n' >"${probe_dir}/probe.cpp"
  local status=0
  g++-16 -std=c++23 "${flag}" \
    "${probe_dir}/probe.cpp" -o "${probe_dir}/probe" >/dev/null 2>&1 || status=1
  rm -rf "${probe_dir}"
  return "${status}"
}

# run_sanitizer_preset <preset> <sanitizer flag> [extra ctest args...]
run_sanitizer_preset() {
  local preset="$1"
  local flag="$2"
  shift 2
  if ! command -v g++-16 >/dev/null 2>&1; then
    echo "g++-16 is unavailable; the hosted Linux legs are authoritative" >&2
    return 77
  fi
  if ! gcc_sanitizer_links "${flag}"; then
    echo "g++-16 cannot link ${flag}: GCC sanitizers are unavailable on this \
host; the hosted Linux legs are authoritative" >&2
    return 77
  fi
  run_preset "${preset}" "$@"
}

# libFuzzer needs a runtime the compiler must ship; AppleClang does not, so the
# gate reports host-unavailable instead of failing a local preflight.
fuzzer_link_available() {
  local probe_dir=""
  probe_dir="$(mktemp -d)" || return 1
  cat >"${probe_dir}/probe.cpp" <<'PROBE'
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
PROBE
  local status=0
  "${CXX:-c++}" -std=c++23 -fsanitize=fuzzer \
    "${probe_dir}/probe.cpp" -o "${probe_dir}/probe" >/dev/null 2>&1 || status=1
  rm -rf "${probe_dir}"
  return "${status}"
}

run_fuzz_replay() {
  if ! fuzzer_link_available; then
    echo "${CXX:-c++} cannot link -fsanitize=fuzzer; the hosted fuzz replay leg is authoritative" >&2
    return 77
  fi
  # SCRY_FUZZ_RUNS=0 makes libFuzzer execute the seed corpus once and exit,
  # which is a deterministic replay rather than a search.
  cmake --preset fuzz -DSCRY_FUZZ_RUNS=0 &&
    cmake --build build/fuzz &&
    ctest --test-dir build/fuzz --output-on-failure -R 'fuzz$'
}

cd "${root_dir}"
run_gate "Doxygen API site" ./scripts/ci-docs.sh
run_gate "core" ./scripts/ci-local.sh
run_gate "clang-tidy" run_tidy
run_gate "ASan + UBSan" run_sanitizer_preset asan -fsanitize=address,undefined
# TSan is where nondeterminism surfaces; the repeat runs live here (QA-008).
run_gate "TSan" run_sanitizer_preset tsan -fsanitize=thread --repeat until-fail:3
run_gate "fuzz corpus replay" run_fuzz_replay

if [[ -n "${skipped_gates}" ]]; then
  echo
  echo "Skipped on this host; hosted CI is authoritative for:"
  printf '%s' "${skipped_gates}" | while IFS= read -r gate; do
    echo "  - ${gate}"
  done
fi

if [[ "${failures}" -ne 0 ]]; then
  echo
  echo "Preflight failed in ${failures} gate(s)." >&2
  exit 1
fi

echo
echo "Preflight passed."
