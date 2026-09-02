#!/usr/bin/env bash

# The clang-tidy leg, run identically by preflight and by hosted CI.
#
# SCRY_CLANG_TOOLING builds only the portable C++23 implementation, which is
# the whole Clang-analyzable surface: the public API is C++26 and GCC-only. The
# ci preset pins g++-16, so the Clang compiler is named explicitly here.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "${root_dir}"

cmake \
  --preset ci \
  --fresh \
  -B build/tidy \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DSCRY_CLANG_TOOLING=ON \
  -DSCRY_ENABLE_CLANG_TIDY=ON \
  -DSCRY_ENABLE_FORMAT_CHECK=OFF
cmake --build build/tidy
