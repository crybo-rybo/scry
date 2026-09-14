#!/usr/bin/env bash

# Shared by just and CI; formatting needs no configured CMake build.
set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly formatter="${CLANG_FORMAT:-clang-format}"
case "${1:---check}" in
  --check) format_args=(--dry-run --Werror) ;;
  --fix) format_args=(-i) ;;
  *)
    echo "Usage: $0 [--check|--fix]" >&2
    exit 2
    ;;
esac

cd "${root_dir}"
# Include new, untracked sources as well as tracked files. NUL delimiters keep
# paths containing spaces intact, and --exclude-standard skips build outputs.
git ls-files --cached --others --exclude-standard -z -- \
  'examples/*.cpp' 'extras/*.cpp' 'extras/*.hpp' 'include/*.hpp' \
  'src/*.cpp' 'src/*.hpp' 'tests/*.cpp' 'tests/*.hpp' 'cmake/probes/*.cpp' |
  while IFS= read -r -d '' source; do
    if [[ -f "${source}" ]]; then
      printf '%s\0' "${source}"
    fi
  done | xargs -0 "${formatter}" "${format_args[@]}"
