#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly analyzer="${root_dir}/scripts/quality_gate.py"
readonly python="${PYTHON:-python3}"
readonly base_ref="${SCRY_BASE_REF:-origin/main}"
readonly head_output="${root_dir}/build/quality"

find_llvm_tool() {
  local tool="$1"
  local compiler_version
  compiler_version="$("${CXX:-clang++}" --version | head -n 1 | grep -Eo '[0-9]+' | head -n 1 || true)"

  for candidate in "${tool}-${compiler_version}" "${tool}"; do
    if [[ -n "${compiler_version}" ]] && command -v "${candidate}" >/dev/null 2>&1; then
      command -v "${candidate}"
      return
    fi
  done
  if command -v xcrun >/dev/null 2>&1 && xcrun -f "${tool}" >/dev/null 2>&1; then
    xcrun -f "${tool}"
    return
  fi
  echo "Unable to find ${tool} for ${CXX:-clang++}" >&2
  return 1
}

readonly llvm_cov="${LLVM_COV:-$(find_llvm_tool llvm-cov)}"
readonly llvm_profdata="${LLVM_PROFDATA:-$(find_llvm_tool llvm-profdata)}"

if ! "${CXX:-clang++}" --version | head -n 1 | grep -q "clang"; then
  echo "quality-gate.sh requires a Clang-family CXX compiler" >&2
  exit 1
fi
if ! "${python}" -m lizard --version >/dev/null 2>&1; then
  echo "quality-gate.sh requires the pinned lizard Python module" >&2
  exit 1
fi
"${python}" -m unittest scripts.test_quality_gate

readonly base_commit="$(git -C "${root_dir}" merge-base "${base_ref}" HEAD)"
readonly temporary_dir="$(mktemp -d "${TMPDIR:-/tmp}/scry-quality.XXXXXX")"
trap 'rm -rf "${temporary_dir}"' EXIT

mkdir -p "${temporary_dir}/base"
git -C "${root_dir}" archive "${base_commit}" | tar -x -C "${temporary_dir}/base"

collect_report() {
  local source_dir="$1"
  local output_dir="$2"
  local build_dir="${output_dir}/build"
  local profile_dir="${output_dir}/profiles"
  local ctest_json="${output_dir}/ctest.json"
  local profile_data="${output_dir}/coverage.profdata"
  local coverage_json="${output_dir}/coverage.json"
  local lizard_csv="${output_dir}/lizard.csv"
  local report_json="${output_dir}/report.json"
  local -a test_binaries=()
  local -a llvm_objects=()

  cmake -E remove_directory "${output_dir}"
  mkdir -p "${profile_dir}"
  CC="${CC:-clang}" CXX="${CXX:-clang++}" cmake \
    -S "${source_dir}" \
    -B "${build_dir}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DSCRY_BUILD_EXAMPLES=ON \
    -DSCRY_BUILD_TESTS=ON \
    -DSCRY_ENABLE_FORMAT_CHECK=OFF \
    -DSCRY_USE_LIBCXX=ON \
    -DSCRY_WARNINGS_AS_ERRORS=ON
  cmake --build "${build_dir}"
  LLVM_PROFILE_FILE="${profile_dir}/%m-%p.profraw" \
    ctest \
      --test-dir "${build_dir}" \
      --output-on-failure \
      --repeat until-fail:3
  ctest --test-dir "${build_dir}" --show-only=json-v1 >"${ctest_json}"

  while IFS= read -r binary; do
    test_binaries+=("${binary}")
  done < <("${python}" "${analyzer}" test-binaries --ctest-json "${ctest_json}")
  if [[ "${#test_binaries[@]}" -eq 0 ]]; then
    echo "No CTest executables were found for coverage" >&2
    return 1
  fi

  "${llvm_profdata}" merge -sparse "${profile_dir}"/*.profraw -o "${profile_data}"
  for binary in "${test_binaries[@]:1}"; do
    llvm_objects+=(--object "${binary}")
  done
  if [[ "${#llvm_objects[@]}" -eq 0 ]]; then
    "${llvm_cov}" export \
      "${test_binaries[0]}" \
      -instr-profile="${profile_data}" >"${coverage_json}"
  else
    "${llvm_cov}" export \
      "${test_binaries[0]}" \
      "${llvm_objects[@]}" \
      -instr-profile="${profile_data}" >"${coverage_json}"
  fi

  (
    cd "${source_dir}"
    "${python}" -m lizard include src examples spikes tests -l cpp --csv
  ) >"${lizard_csv}"
  "${python}" "${analyzer}" analyze \
    --source-root "${source_dir}" \
    --coverage-json "${coverage_json}" \
    --lizard-csv "${lizard_csv}" \
    --output "${report_json}"
}

collect_report "${temporary_dir}/base" "${temporary_dir}/base-output"
collect_report "${root_dir}" "${head_output}"

"${python}" "${analyzer}" gate \
  --repository "${root_dir}" \
  --base-ref "${base_commit}" \
  --base-report "${temporary_dir}/base-output/report.json" \
  --head-report "${head_output}/report.json" \
  --minimum-diff-coverage 90
