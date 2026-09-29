#!/usr/bin/env bash

# The per-commit libFuzzer corpus replay for the kernel's fuzz targets, run
# identically by scripts/ci.sh and hosted CI. Each registered fuzz test executes
# its seed corpus once and exits, which is a deterministic replay rather than a
# search (cmake/ScryFuzz.cmake). The fuzz targets over the reflective side of the
# library are GCC corpus replays in the ordinary test build, so the core and
# sanitizer legs run them.
#
# The fuzz preset sets SCRY_CLANG_TOOLING, so the compiler must be a
# Clang-family one carrying libFuzzer.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

cd "${root_dir}"

cmake --preset fuzz -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
cmake --build build/fuzz
ctest --test-dir build/fuzz --output-on-failure
