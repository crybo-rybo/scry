#!/usr/bin/env bash

# Build and run the unit and integration suites without a live model.
# Additional arguments are forwarded to ctest, for example -R 'runtime\.'.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly root_dir

cd "${root_dir}"
cmake --preset dev -DCMAKE_CXX_COMPILER="${CXX:-g++-16}" -DSCRY_BUILD_TESTS=ON
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure --no-tests=error "$@"
