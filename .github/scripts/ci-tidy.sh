#!/usr/bin/env bash

# The clang-tidy leg, run identically by scripts/ci.sh and by hosted CI.
#
# SCRY_CLANG_TOOLING builds only the C++23 kernel (src/kernel/), which is the
# whole Clang-analyzable surface: the public API and the rest of src/ are C++26
# and GCC-only. The ci preset pins g++-16, so the Clang compiler is named
# explicitly here.
#
# Extra arguments are forwarded to the configure step; the hosted leg passes
# -DSCRY_CLANG_TIDY_EXECUTABLE=clang-tidy-21 to match its versioned compiler.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

cd "${root_dir}"

cmake \
  --preset ci \
  --fresh \
  -B build/tidy \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DSCRY_CLANG_TOOLING=ON \
  -DSCRY_ENABLE_CLANG_TIDY=ON \
  "$@"
cmake --build build/tidy
