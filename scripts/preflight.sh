#!/usr/bin/env bash

# Local equivalent of the per-commit CI ring: documentation, core, clang-tidy,
# sanitizers, and the fuzz corpus replay. Long fuzz runs, the showcase, and the
# local-model smoke live in the scheduled/manual nightly workflow.
#
# Every gate runs the same scripts/ci-*.sh script the hosted leg runs; the only
# thing that lives here is the host-capability probe in front of it.
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
  # A missing toolchain is a host capability, not a failing gate, so these
  # probes return 77 like the sanitizer and fuzz ones do.
  if ! PATH="${tidy_path}" command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy is unavailable; the hosted clang-tidy leg is authoritative" >&2
    return 77
  fi
  if ! PATH="${tidy_path}" command -v clang >/dev/null 2>&1 ||
    ! PATH="${tidy_path}" command -v clang++ >/dev/null 2>&1; then
    echo "Clang is unavailable; the clang-tidy leg requires its matching \
compiler and the hosted leg is authoritative" >&2
    return 77
  fi
  PATH="${tidy_path}" ./scripts/ci-tidy.sh
}

# Doxygen renders the API site through dot, and neither ships with the compiler
# toolchain, so a host without them reports unavailable rather than failing.
run_docs() {
  if ! command -v doxygen >/dev/null 2>&1 || ! command -v dot >/dev/null 2>&1; then
    echo "doxygen or dot is unavailable; the hosted documentation leg is \
authoritative" >&2
    return 77
  fi
  ./scripts/ci-docs.sh
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

# run_sanitizer_leg <asan|tsan> <sanitizer flag>
run_sanitizer_leg() {
  local leg="$1"
  local flag="$2"
  if ! command -v g++-16 >/dev/null 2>&1; then
    echo "g++-16 is unavailable; the hosted Linux legs are authoritative" >&2
    return 77
  fi
  if ! gcc_sanitizer_links "${flag}"; then
    echo "g++-16 cannot link ${flag}: GCC sanitizers are unavailable on this \
host; the hosted Linux legs are authoritative" >&2
    return 77
  fi
  ./scripts/ci-sanitizer.sh "${leg}"
}

# libFuzzer needs a runtime the compiler must ship; AppleClang does not, so the
# gate reports host-unavailable instead of failing a local preflight. The
# probe uses the same compiler default as scripts/ci-fuzz-replay.sh.
fuzzer_link_available() {
  local probe_dir=""
  probe_dir="$(mktemp -d)" || return 1
  cat >"${probe_dir}/probe.cpp" <<'PROBE'
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
PROBE
  local status=0
  "${CXX:-clang++}" -std=c++23 -fsanitize=fuzzer \
    "${probe_dir}/probe.cpp" -o "${probe_dir}/probe" >/dev/null 2>&1 || status=1
  rm -rf "${probe_dir}"
  return "${status}"
}

run_fuzz_replay() {
  if ! fuzzer_link_available; then
    echo "${CXX:-clang++} cannot link -fsanitize=fuzzer; the hosted fuzz replay leg is authoritative" >&2
    return 77
  fi
  ./scripts/ci-fuzz-replay.sh
}

cd "${root_dir}"
run_gate "Doxygen API site" run_docs
run_gate "core" ./scripts/ci-local.sh
run_gate "clang-tidy" run_tidy
run_gate "ASan + UBSan" run_sanitizer_leg asan -fsanitize=address,undefined
# TSan is where nondeterminism surfaces; ci-sanitizer.sh puts the repeat runs
# on that leg (QA-008).
run_gate "TSan" run_sanitizer_leg tsan -fsanitize=thread
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
