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
python3 -m lizard include src examples tests extras -l cpp -C 15 -a 6
unlinked_todos="$(
  git grep -nE '//[[:space:]]*TODO\b' -- include src examples tests extras |
    grep -vE 'https?://|#[0-9]+' || true
)"
if [[ -n "${unlinked_todos}" ]]; then
  echo "TODO comments must link an issue:" >&2
  echo "${unlinked_todos}" >&2
  exit 1
fi
if [[ "${format_check}" == "1" ]]; then
  ./scripts/format.sh --check
fi
cmake --preset ci "$@"
cmake --build "${build_dir}"

ctest \
  --test-dir "${build_dir}" \
  --output-on-failure
cmake -E remove_directory "${stage_dir}"
cmake --install "${build_dir}" --prefix "${stage_dir}"
# Showcase code and dependencies must stay out of the installed library.
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

# The installed package must be usable by a downstream project: the consumer
# exercises the explicit-schema surface and the reflected surface through
# scry::scry alone, so the reflected API cannot silently stop being installed.
cmake -E remove_directory "${consumer_dir}"
cmake \
  -S tests/package_consumer \
  -B "${consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="${CXX:-g++-16}" \
  -DCMAKE_PREFIX_PATH="${stage_dir}"
cmake --build "${consumer_dir}"
"${consumer_dir}/scry_package_consumer"
