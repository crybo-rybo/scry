#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/docs"

cmake --fresh -S "${root_dir}/docs" -B "${build_dir}" -G Ninja
cmake --build "${build_dir}" --target scry_docs

test -s "${build_dir}/html/index.html"
