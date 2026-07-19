#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/reflection-gcc16-coverage"
readonly python="${PYTHON:-python3}"
readonly gcovr_version="8.6"
readonly minimum_branch_coverage="95"
# GCC's decision analysis still counts the one GCOVR_EXCL_LINE-marked
# compiler-generated switch on the reflected-enum decoder, so the decision
# floor sits below the ~89% unadjusted result rather than the ~100% adjusted
# one. The fuzzier floor is the price of gating with stock gcovr instead of a
# bespoke exclusion validator (ADR 0011).
readonly minimum_decision_coverage="85"
readonly minimum_function_coverage="95"
readonly codec_summary="${build_dir}/reflection-codec-coverage-summary.json"
readonly bridge_summary="${build_dir}/reflection-bridge-coverage-summary.json"

if ! command -v g++-16 >/dev/null 2>&1; then
  echo "g++-16 is required for reflection coverage" >&2
  exit 1
fi
if ! command -v gcov-16 >/dev/null 2>&1; then
  echo "gcov-16 is required for reflection coverage" >&2
  exit 1
fi
if ! "${python}" -m gcovr --version 2>/dev/null |
  grep -Fxq "gcovr ${gcovr_version}"; then
  echo "gcovr ${gcovr_version} is required for reflection coverage" >&2
  exit 1
fi

cd "${root_dir}"
cmake -E remove_directory "${build_dir}"
cmake \
  --preset reflection-gcc16 \
  --fresh \
  -B "${build_dir}" \
  -DCMAKE_CXX_FLAGS="--coverage -fprofile-abs-path -O0" \
  -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
  -DSCRY_ENABLE_FORMAT_CHECK=OFF
cmake --build "${build_dir}" --target scry_reflection_tests
ctest \
  --test-dir "${build_dir}" \
  --output-on-failure \
  --label-regex reflection

readonly -a gcovr_common=(
  "${python}"
  -m
  gcovr
  --root
  "${root_dir}"
  --object-directory
  "${build_dir}"
  --gcov-executable
  gcov-16
  --exclude-unreachable-branches
  --exclude-throw-branches
  --exclude-noncode-lines
  --merge-lines
)

"${gcovr_common[@]}" \
  --filter "^src/reflection/json_bridge\\.cpp$" \
  --fail-under-branch "${minimum_branch_coverage}" \
  --json-summary="${bridge_summary}" \
  --json-summary-pretty \
  --txt-metric branch \
  --txt -

"${gcovr_common[@]}" \
  --filter "^include/scry/detail/reflection_codec\\.hpp$" \
  --decisions \
  --fail-under-decision "${minimum_decision_coverage}" \
  --fail-under-function "${minimum_function_coverage}" \
  --json-summary="${codec_summary}" \
  --json-summary-pretty \
  --txt-metric decision \
  --txt -
