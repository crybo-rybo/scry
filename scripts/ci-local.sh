#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/ci"
readonly stage_dir="${root_dir}/build/stage"
readonly consumer_dir="${root_dir}/build/package-consumer"

cd "${root_dir}"

# Whitespace errors across the whole branch plus uncommitted edits. Without an
# origin/main to diff against (a shallow checkout), only uncommitted edits.
whitespace_base="$(git merge-base HEAD origin/main 2>/dev/null || echo HEAD)"
git diff --check "${whitespace_base}"
python3 -m lizard include src testing examples tests extras -l cpp -C 15 -a 6
unlinked_todos="$(
  git grep -nE '//[[:space:]]*TODO\b' -- include src testing examples tests extras |
    grep -vE 'https?://|#[0-9]+' || true
)"
if [[ -n "${unlinked_todos}" ]]; then
  echo "TODO comments must link an issue:" >&2
  echo "${unlinked_todos}" >&2
  exit 1
fi
cmake --preset ci "$@"
cmake --build "${build_dir}"

ctest \
  --test-dir "${build_dir}" \
  --output-on-failure
cmake -E remove_directory "${stage_dir}"
cmake --install "${build_dir}" --prefix "${stage_dir}"

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
