#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/ci"
readonly stage_dir="${root_dir}/build/stage"
readonly consumer_dir="${root_dir}/build/package-consumer"

cd "${root_dir}"

# SCRY_FORMAT_CHECK=0 skips formatting so legs without the pinned
# clang-format (e.g. macOS CI; the dedicated format job owns that gate)
# still run everything else.
readonly format_check="${SCRY_FORMAT_CHECK:-1}"

git diff --check
python3 -m unittest scripts.test_quality_gate
python3 -m lizard include examples spikes tests -l cpp -C 15 -L 60 -a 6
python3 -m lizard scripts -l python -C 15 -L 60 -a 6
if [[ "${format_check}" == "1" ]]; then
  cmake --preset ci "$@"
  cmake --build "${build_dir}" --target all format-check
else
  cmake --preset ci -DSCRY_ENABLE_FORMAT_CHECK=OFF "$@"
  cmake --build "${build_dir}" --target all
fi
ctest \
  --test-dir "${build_dir}" \
  --output-on-failure \
  --repeat until-fail:3
cmake --install "${build_dir}" --prefix "${stage_dir}"
cmake \
  -S tests/package_consumer \
  -B "${consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${stage_dir}"
cmake --build "${consumer_dir}"
