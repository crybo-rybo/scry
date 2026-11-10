#!/usr/bin/env bash

# clang-tidy over the C++23 kernel (the rest of src/ uses C++26 reflection,
# which clang-tidy cannot parse),
# plus the repository rules clang-tidy does not cover.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
clang_tidy="${CLANG_TIDY:-clang-tidy}"
status=0

# Resolve dependency headers through CMake, including installed packages and
# FetchContent overrides. No dependency sources are linted in this repository.
lint_build_dir="${SCRY_LINT_BUILD_DIR:-build/lint}"
cmake -S . -B "${lint_build_dir}" -G Ninja \
  -DCMAKE_CXX_COMPILER="${CXX:-g++-16}" \
  -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
json_includes=()
while IFS= read -r include_dir; do
  if [[ -n "${include_dir}" ]]; then
    json_includes+=(-isystem "${include_dir}")
  fi
done < "${lint_build_dir}/scrydantic-json-includes.txt"

kernel_sources=()
while IFS= read -r source; do kernel_sources+=("${source}"); done \
  < <(git ls-files 'src/kernel/*.cpp')
"${clang_tidy}" --quiet "${kernel_sources[@]}" -- -std=c++23 -Iinclude -Isrc \
  "${json_includes[@]}" || status=1

# The kernel may include only these public headers, Scrydantic's C++23 JSON
# core headers, and no src/ header outside src/kernel/.
if git grep -nE '#[[:space:]]*include[[:space:]]*[<"](scry|scrydantic|core|machine|provider|reflection|runtime)/' \
  -- 'src/kernel/' |
  grep -vE '<scry/(config|error|turn_id|unique_function)\.hpp>' |
  grep -vE '<scrydantic/(error|json)\.hpp>'; then
  echo "error: the kernel includes a header outside its boundary" >&2
  status=1
fi

if git grep -nE '//[[:space:]]*TODO([^[:alnum:]_]|$)' -- include src testing examples tests extras |
  grep -vE 'https?://|#[0-9]+'; then
  echo "error: TODO comments must link an issue" >&2
  status=1
fi

exit "${status}"
