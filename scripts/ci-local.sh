#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/ci"
readonly stage_dir="${root_dir}/build/stage"
readonly consumer_dir="${root_dir}/build/package-consumer"

cd "${root_dir}"

git diff --check
python3 -m lizard include examples spikes tests -l cpp -C 15 -L 60 -a 6
cmake --preset ci "$@"
cmake --build "${build_dir}" --target all format-check
ctest --test-dir "${build_dir}" --output-on-failure
cmake --install "${build_dir}" --prefix "${stage_dir}"
cmake \
  -S tests/package_consumer \
  -B "${consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${stage_dir}"
cmake --build "${consumer_dir}"
