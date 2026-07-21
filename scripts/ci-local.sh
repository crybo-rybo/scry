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
python3 -m lizard include src examples tests -l cpp -C 15 -a 6
unlinked_todos="$(
  git grep -nE '//[[:space:]]*TODO\b' -- include src examples tests |
    grep -vE 'https?://|#[0-9]+' || true
)"
if [[ -n "${unlinked_todos}" ]]; then
  echo "TODO comments must link an issue:" >&2
  echo "${unlinked_todos}" >&2
  exit 1
fi
if [[ "${format_check}" == "1" ]]; then
  cmake --preset ci "$@"
  cmake --build "${build_dir}" --target all format-check
else
  cmake --preset ci -DSCRY_ENABLE_FORMAT_CHECK=OFF "$@"
  cmake --build "${build_dir}" --target all
fi
ctest \
  --test-dir "${build_dir}" \
  --output-on-failure
cmake -E remove_directory "${stage_dir}"
cmake --install "${build_dir}" --prefix "${stage_dir}"
for reflection_artifact in \
  "${stage_dir}/include/scry/reflection.hpp" \
  "${stage_dir}/include/scry/detail" \
  "${stage_dir}/lib/libscry_reflection.a" \
  "${stage_dir}/lib/cmake/scry/scryReflectionTargets.cmake"; do
  if [[ -e "${reflection_artifact}" ]]; then
    echo "Reflection artifact leaked into the core package: ${reflection_artifact}" >&2
    exit 1
  fi
done
cmake -E remove_directory "${consumer_dir}"
cmake \
  -S tests/package_consumer \
  -B "${consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="${stage_dir}"
cmake --build "${consumer_dir}"
"${consumer_dir}/scry_package_consumer"
