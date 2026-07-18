#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/showcase"
readonly stage_dir="${root_dir}/build/showcase-stage"
readonly consumer_dir="${root_dir}/build/showcase-package-consumer"
readonly default_build_dir="${root_dir}/build/showcase-default-off"
readonly imgui_commit="8936b58fe26e8c3da834b8f60b06511d537b4c63"

cd "${root_dir}"

cmake -E remove_directory "${default_build_dir}"
cmake \
  -S "${root_dir}" \
  -B "${default_build_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSCRY_BUILD_EXAMPLES=OFF \
  -DSCRY_BUILD_TESTS=OFF \
  -DSCRY_ENABLE_FORMAT_CHECK=OFF
if find "${default_build_dir}" -iname '*imgui*' -print -quit | grep -q .; then
  echo "The default-OFF configure discovered or populated Dear ImGui" >&2
  exit 1
fi
if cmake --build "${default_build_dir}" --target help |
  grep -E -q 'scry_imgui|scry_npc_showcase'; then
  echo "The default-OFF configure exposed a showcase target" >&2
  exit 1
fi

cmake --preset showcase "$@"
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

cmake -E remove_directory "${stage_dir}"
cmake --install "${build_dir}" --prefix "${stage_dir}"

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
  -DCMAKE_PREFIX_PATH="${stage_dir}"
cmake --build "${consumer_dir}"
"${consumer_dir}/scry_package_consumer"

echo "Showcase build, tests, headless frame, and package audit passed."
