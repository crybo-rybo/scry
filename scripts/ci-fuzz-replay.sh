#!/usr/bin/env bash

# The per-commit fuzz corpus replay, run identically by preflight and by hosted
# CI. Each registered fuzz test executes its seed corpus once and exits, which
# is a deterministic replay rather than a search (cmake/ScryFuzz.cmake). The
# long searching runs live in scripts/ci-nightly-fuzz.sh.
#
# The fuzz preset sets SCRY_CLANG_TOOLING, so the compiler must be a
# Clang-family one carrying libFuzzer.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "${root_dir}"

cmake --preset fuzz -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
cmake --build build/fuzz
ctest --test-dir build/fuzz --output-on-failure
