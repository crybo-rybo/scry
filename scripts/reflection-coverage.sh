#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/reflection-gcc16-coverage"
readonly python="${PYTHON:-python3}"
readonly gcovr_version="8.6"
readonly minimum_coverage="95"
readonly codec_detail="${build_dir}/reflection-codec-coverage.json"
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
"${python}" -m unittest scripts.test_reflection_coverage_gate
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
readonly -a runtime_filters=(
  --filter
  "^include/scry/detail/reflection_codec\\.hpp$"
  --filter
  "^src/reflection/json_bridge\\.cpp$"
)

# GCC's raw CFG arcs include duplicated template-instantiation, destructor, and
# exception-flow arcs that do not map one-to-one to C++ source decisions. Keep
# that diagnostic visible while gating the stable, non-template bridge directly.
"${gcovr_common[@]}" \
  "${runtime_filters[@]}" \
  --exclude-pattern-prefix "SCRY_RAW_COVERAGE" \
  --txt-metric branch \
  --txt -

"${gcovr_common[@]}" \
  --filter "^src/reflection/json_bridge\\.cpp$" \
  --fail-under-branch "${minimum_coverage}" \
  --json-summary="${bridge_summary}" \
  --json-summary-pretty \
  --txt-metric branch \
  --txt -

# gcovr's source-decision analysis is format-sensitive, so both gcovr and GCC
# are pinned and the repository's clang-format gate owns the input form. Print
# gcovr's unadjusted decision result before the checked validator removes the
# one explicitly marked GCC-generated switch on decode_enum's definition line.
"${gcovr_common[@]}" \
  --filter "^include/scry/detail/reflection_codec\\.hpp$" \
  --decisions \
  --json="${codec_detail}" \
  --json-pretty \
  --json-summary="${codec_summary}" \
  --json-summary-pretty \
  --txt-metric decision \
  --txt -

"${python}" scripts/reflection_coverage_gate.py \
  --coverage-json "${codec_detail}" \
  --path "include/scry/detail/reflection_codec.hpp" \
  --minimum-decision-coverage "${minimum_coverage}" \
  --minimum-function-coverage 100
