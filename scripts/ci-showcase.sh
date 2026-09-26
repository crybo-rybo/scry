#!/usr/bin/env bash

# Builds the standalone showcase.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/showcase"
readonly cxx_compiler="${CXX:-g++-16}"

cd "${root_dir}"

cmake \
  -S extras/showcase \
  -B "${build_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="${cxx_compiler}" \
  "$@"
cmake --build "${build_dir}"

echo "Showcase build passed."
