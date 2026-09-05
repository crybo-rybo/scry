#!/usr/bin/env bash

# Builds and tests the standalone showcase project under extras/showcase, then
# audits that nothing it adds can reach the installed library package. The
# showcase is not part of the root build, so the audit configures and installs
# the root separately and looks for leaked artifacts there.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/showcase"
readonly stage_build_dir="${root_dir}/build/showcase-root"
readonly stage_dir="${root_dir}/build/showcase-stage"
readonly consumer_dir="${root_dir}/build/showcase-package-consumer"
readonly imgui_commit="f1cc2ae15e53a861a874c3034aae6798fde194ab"
readonly cxx_compiler="${CXX:-g++-16}"

cd "${root_dir}"

cmake \
  -S extras/showcase \
  -B "${build_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="${cxx_compiler}" \
  "$@"
readonly fetched_imgui_commit="$(
  git -C "${build_dir}/_deps/imgui-src" rev-parse HEAD
)"
if [[ "${fetched_imgui_commit}" != "${imgui_commit}" ]]; then
  echo "Dear ImGui checkout does not match the pinned commit" >&2
  exit 1
fi
cmake --build "${build_dir}"
ctest \
  --test-dir "${build_dir}" \
  --output-on-failure \
  --repeat until-fail:3 \
  -L showcase

# The root build knows nothing about the showcase; prove it by installing the
# library on its own and auditing the prefix.
cmake -E remove_directory "${stage_build_dir}"
cmake \
  -S "${root_dir}" \
  -B "${stage_build_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="${cxx_compiler}" \
  -DSCRY_BUILD_EXAMPLES=OFF \
  -DSCRY_BUILD_TESTS=OFF \
  -DSCRY_ENABLE_FORMAT_CHECK=OFF
cmake --build "${stage_build_dir}"
cmake -E remove_directory "${stage_dir}"
cmake --install "${stage_build_dir}" --prefix "${stage_dir}"

if find "${stage_dir}" -type f \
  \( -iname '*imgui*' -o -iname '*showcase*' -o -iname '*npc*' \) \
  -print -quit | grep -q .; then
  echo "Showcase artifact leaked into the installed package" >&2
  exit 1
fi

if grep -R -E -i 'imgui|scry_showcase|scry_npc' \
  "${stage_dir}/lib/cmake/scry" >/dev/null; then
  echo "Showcase dependency leaked into the installed CMake package" >&2
  exit 1
fi

cmake -E remove_directory "${consumer_dir}"
cmake \
  -S tests/package_consumer \
  -B "${consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="${cxx_compiler}" \
  -DCMAKE_PREFIX_PATH="${stage_dir}"
cmake --build "${consumer_dir}"
"${consumer_dir}/scry_package_consumer"

echo "Showcase build, tests, headless frame, and package audit passed."
