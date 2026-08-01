#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/docs"

for command in cmake doxygen dot; do
  if ! command -v "${command}" >/dev/null 2>&1; then
    echo "Required documentation command is unavailable: ${command}" >&2
    exit 1
  fi
done

cmake --fresh -S "${root_dir}/docs" -B "${build_dir}" -G Ninja
cmake --build "${build_dir}" --target scry_docs

test -s "${build_dir}/html/index.html"
