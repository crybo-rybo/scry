#!/usr/bin/env bash

# clang-tidy over the C++23 kernel (the rest of src/ is C++26 reflection, which
# clang-tidy cannot parse), plus the repository rules clang-tidy does not cover.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
clang_tidy="${CLANG_TIDY:-clang-tidy}"
status=0

kernel_sources=()
while IFS= read -r source; do kernel_sources+=("${source}"); done \
  < <(git ls-files 'src/kernel/*.cpp')
"${clang_tidy}" --quiet "${kernel_sources[@]}" -- -std=c++23 -Iinclude -Isrc || status=1

# The kernel may include only these public headers and no src/ header outside
# src/kernel/.
if git grep -nE '#[[:space:]]*include[[:space:]]*[<"](scry|core|machine|provider|reflection|runtime)/' \
  -- 'src/kernel/' |
  grep -vE '<scry/(config|error|json|turn_id|unique_function)\.hpp>'; then
  echo "error: the kernel includes a header outside its boundary" >&2
  status=1
fi

if git grep -nE '//[[:space:]]*TODO([^[:alnum:]_]|$)' -- include src testing examples tests extras |
  grep -vE 'https?://|#[0-9]+'; then
  echo "error: TODO comments must link an issue" >&2
  status=1
fi

exit "${status}"
