#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/reflection-gcc16"
readonly stage_dir="${root_dir}/build/stage-reflection"
readonly consumer_dir="${root_dir}/build/package-consumer-reflection"
readonly core_consumer_dir="${root_dir}/build/package-consumer-core-on-reflection"
readonly sanitizer_build_dir="${root_dir}/build/reflection-gcc16-asan-ubsan"

if ! command -v g++-16 >/dev/null 2>&1; then
  echo "g++-16 is required for the supported reflection component" >&2
  exit 1
fi
if ! command -v g++-14 >/dev/null 2>&1; then
  echo "g++-14 is required to prove the core surface of the reflection-enabled \
package stays C++23 (TOOL-003)" >&2
  exit 1
fi

cd "${root_dir}"

cmake --preset reflection-gcc16 --fresh
cmake --build "${build_dir}"
ctest \
  --test-dir "${build_dir}" \
  --output-on-failure

cmake -E remove_directory "${stage_dir}"
cmake --install "${build_dir}" --prefix "${stage_dir}"

cmake -E remove_directory "${consumer_dir}"
cmake \
  -S tests/package_consumer_reflection \
  -B "${consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-16 \
  -DCMAKE_PREFIX_PATH="${stage_dir}"
cmake --build "${consumer_dir}"
"${consumer_dir}/scry_reflection_package_consumer"

# TOOL-003: the same reflection-enabled installation must present a pure C++23
# surface to a core-only consumer. Compiling with a non-reflection compiler
# proves it — any -std=c++26/-freflection or Glaze leak into scry::scry fails
# this compile, and the consumer's configure rejects a leaked reflection
# component. Compile-only: linking the GCC 16-built archive needs GCC 16's
# libstdc++ whatever the package exposes, so a link step adds no evidence.
cmake -E remove_directory "${core_consumer_dir}"
cmake \
  -S tests/package_consumer \
  -B "${core_consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_PREFIX_PATH="${stage_dir}" \
  -DSCRY_CONSUMER_COMPILE_ONLY=ON
cmake --build "${core_consumer_dir}"

cmake \
  --preset reflection-gcc16 \
  --fresh \
  -B "${sanitizer_build_dir}" \
  -DSCRY_SANITIZER=address-undefined
cmake --build "${sanitizer_build_dir}"
ctest \
  --test-dir "${sanitizer_build_dir}" \
  --output-on-failure \
  --label-regex reflection
