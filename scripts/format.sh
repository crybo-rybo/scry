#!/usr/bin/env bash

# clang-format over the C++ sources: --check (default) or --fix.
#
# The version is pinned here and fetched from PyPI by uv, so every machine and
# CI run the identical binary; versions disagree on reflection annotations.
# Set CLANG_FORMAT to use a different binary.

set -euo pipefail

readonly version="23.1.2"

case "${1:---check}" in
  --check) format_args=(--dry-run --Werror) ;;
  --fix) format_args=(-i) ;;
  *)
    echo "Usage: $0 [--check|--fix]" >&2
    exit 2
    ;;
esac

if [[ -n "${CLANG_FORMAT:-}" ]]; then
  formatter=("${CLANG_FORMAT}")
elif command -v uvx >/dev/null 2>&1; then
  formatter=(uvx --quiet "clang-format@${version}")
else
  echo "Install uv (https://docs.astral.sh/uv/) or set CLANG_FORMAT to clang-format ${version}" >&2
  exit 1
fi

cd "$(dirname "${BASH_SOURCE[0]}")/.."
# Include new, untracked sources as well as tracked files. NUL delimiters keep
# paths containing spaces intact, and --exclude-standard skips build outputs.
git ls-files --cached --others --exclude-standard -z -- \
  'examples/*.cpp' 'extras/*.cpp' 'extras/*.hpp' 'include/*.hpp' \
  'src/*.cpp' 'src/*.hpp' 'testing/*.cpp' 'tests/*.cpp' 'tests/*.hpp' |
  while IFS= read -r -d '' source; do
    if [[ -f "${source}" ]]; then
      printf '%s\0' "${source}"
    fi
  done | xargs -0 "${formatter[@]}" "${format_args[@]}"
