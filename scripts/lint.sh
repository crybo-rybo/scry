#!/usr/bin/env bash

# clang-tidy over the C++23 kernel and Scrydantic's C++23 JSON core (the rest of
# src/ and of scrydantic/ is C++26 reflection, which clang-tidy cannot parse),
# plus the repository rules clang-tidy does not cover.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
clang_tidy="${CLANG_TIDY:-clang-tidy}"
status=0

kernel_sources=()
while IFS= read -r source; do kernel_sources+=("${source}"); done \
  < <(git ls-files 'src/kernel/*.cpp')
"${clang_tidy}" --quiet "${kernel_sources[@]}" -- -std=c++23 -Iinclude -Isrc \
  -Iscrydantic/include || status=1

json_core_sources=()
while IFS= read -r source; do json_core_sources+=("${source}"); done \
  < <(git ls-files 'scrydantic/src/json/*.cpp')
"${clang_tidy}" --quiet "${json_core_sources[@]}" -- -std=c++23 \
  -Iscrydantic/include -Iscrydantic/src || status=1

# The kernel may include only these public headers, Scrydantic's C++23 JSON
# core headers, and no src/ header outside src/kernel/.
if git grep -nE '#[[:space:]]*include[[:space:]]*[<"](scry|scrydantic|core|machine|provider|reflection|runtime)/' \
  -- 'src/kernel/' |
  grep -vE '<scry/(config|error|turn_id|unique_function)\.hpp>' |
  grep -vE '<scrydantic/(error|json|detail/json_text)\.hpp>'; then
  echo "error: the kernel includes a header outside its boundary" >&2
  status=1
fi

# Scrydantic's JSON core may include only its own public JSON headers and other
# headers of the core.
if git grep -nE '#[[:space:]]*include[[:space:]]*[<"][a-z_]+/' -- 'scrydantic/src/json/' |
  grep -vE '"json/[a-z_]+\.hpp"' |
  grep -vE '<scrydantic/(error|json|detail/json_text)\.hpp>'; then
  echo "error: the JSON core includes a header outside its boundary" >&2
  status=1
fi

# Scrydantic is self-contained: nothing under scrydantic/ includes a Scry
# header, a header from Scry's src/, or libcurl, and nothing links libcurl or
# Threads.
if git grep -nE '#[[:space:]]*include[[:space:]]*(<(scry|curl)/|"(core|kernel|machine|provider|reflection|runtime)/)' \
  -- 'scrydantic/*.cpp' 'scrydantic/*.hpp'; then
  echo "error: scrydantic/ includes a header outside its boundary" >&2
  status=1
fi
if git grep -nE '(CURL|Threads)::' -- 'scrydantic/*CMakeLists.txt' 'scrydantic/*.cmake' \
  'scrydantic/*.cmake.in'; then
  echo "error: scrydantic/ links a dependency outside its boundary" >&2
  status=1
fi

if git grep -nE '//[[:space:]]*TODO([^[:alnum:]_]|$)' -- include src testing examples tests extras scrydantic |
  grep -vE 'https?://|#[0-9]+'; then
  echo "error: TODO comments must link an issue" >&2
  status=1
fi

exit "${status}"
