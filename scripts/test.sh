#!/usr/bin/env bash

# Build and run the unit and integration tests; no live model needed.
# PRESET selects dev (default), asan, or tsan. Extra arguments go to ctest,
# for example -R 'runtime\.'.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
preset="${PRESET:-dev}"

cmake --preset "${preset}" -DCMAKE_CXX_COMPILER="${CXX:-g++-16}"
cmake --build "build/${preset}"
ctest --test-dir "build/${preset}" --parallel --output-on-failure --no-tests=error "$@"
