#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly analyzer="${root_dir}/scripts/quality_gate.py"
readonly python="${PYTHON:-python3}"
readonly base_ref="${SCRY_BASE_REF:-origin/main}"
readonly head_output="${root_dir}/build/quality"
readonly coverage_flags="-fprofile-instr-generate -fcoverage-mapping -fprofile-update=atomic"
readonly direct_test_timeout_seconds=300

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
  local install_dir="${output_dir}/install"
  local mapper_build_dir="${output_dir}/coverage-mapper"
  local mapper="${mapper_build_dir}/scry_quality_coverage_mapper"
  local ctest_json="${output_dir}/ctest.json"
  local profile_data="${output_dir}/coverage.profdata"
  local coverage_json="${output_dir}/coverage.json"
  local lizard_csv="${output_dir}/lizard.csv"
  local report_json="${output_dir}/report.json"
  local canonical_build_dir
  local -a test_binaries=()
  local -a test_working_directories=()

  cmake -E remove_directory "${output_dir}"
  mkdir -p "${profile_dir}"
  CC="${CC:-clang}" CXX="${CXX:-clang++}" cmake \
    -S "${source_dir}" \
    -B "${build_dir}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="${coverage_flags}" \
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DSCRY_BUILD_EXAMPLES=ON \
    -DSCRY_BUILD_TESTS=ON \
    -DSCRY_ENABLE_FORMAT_CHECK=OFF \
    -DSCRY_USE_LIBCXX=ON \
    -DSCRY_WARNINGS_AS_ERRORS=ON
  cmake --build "${build_dir}"
  ctest \
    --test-dir "${build_dir}" \
    --output-on-failure \
    --repeat until-fail:3
  ctest --test-dir "${build_dir}" --show-only=json-v1 >"${ctest_json}"
  cmake --install "${build_dir}" --prefix "${install_dir}"
  CC="${CC:-clang}" CXX="${CXX:-clang++}" cmake \
    -S "${root_dir}/scripts/quality_coverage_mapper" \
    -B "${mapper_build_dir}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="${coverage_flags}" \
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DCMAKE_PREFIX_PATH="${install_dir}" \
    -DSCRY_SOURCE_ROOT="${source_dir}"
  cmake --build "${mapper_build_dir}"

  canonical_build_dir="$(cd "${build_dir}" && pwd -P)"
  while IFS=$'\t' read -r binary working_directory; do
    if [[ "${binary}" == "${canonical_build_dir}/"* ]]; then
      test_binaries+=("${binary}")
      test_working_directories+=("${working_directory}")
    fi
  done < <(
    "${python}" "${analyzer}" test-binaries --ctest-json "${ctest_json}" |
      LC_ALL=C sort
  )
  if [[ "${#test_binaries[@]}" -eq 0 ]]; then
    echo "No native CTest executables were found for coverage" >&2
    return 1
  fi

  cmake -E remove_directory "${profile_dir}"
  mkdir -p "${profile_dir}"
  local index=0
  local prefix
  local repeat
  for binary in "${test_binaries[@]}"; do
    printf -v prefix "%03d" "${index}"
    for repeat in 0 1 2; do
      if [[ "$(basename "${binary}")" == "scry_public_api_contract" ]]; then
        LLVM_PROFILE_FILE="${profile_dir}/${prefix}-${repeat}-%m-%p.profraw" \
          "${python}" "${analyzer}" run-test-binary \
            --working-directory "${test_working_directories[index]}" \
            --timeout-seconds "${direct_test_timeout_seconds}" \
            -- "${binary}"
      else
        LLVM_PROFILE_FILE="${profile_dir}/${prefix}-${repeat}-%m-%p.profraw" \
          "${python}" "${analyzer}" run-test-binary \
            --working-directory "${test_working_directories[index]}" \
            --timeout-seconds "${direct_test_timeout_seconds}" \
            -- "${binary}" --order lex --rng-seed 1
      fi
    done
    index=$((index + 1))
  done

  printf -v prefix "%03d" "${index}"
  for repeat in 0 1 2; do
    LLVM_PROFILE_FILE="${profile_dir}/${prefix}-${repeat}-%m-%p.profraw" \
      ctest \
        --test-dir "${build_dir}" \
        --output-on-failure \
        -R "^integration\\.self-signed TLS is rejected unless explicitly disabled$"
  done
  LLVM_PROFILE_FILE="${profile_dir}/999-%m-%p.profraw" "${mapper}"

  "${llvm_profdata}" merge -sparse "${profile_dir}"/*.profraw -o "${profile_data}"
  "${llvm_cov}" export \
    "${mapper}" \
    -instr-profile="${profile_data}" >"${coverage_json}"

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
