#!/usr/bin/env bash

# One sanitizer leg, run identically by preflight and by hosted CI.
#
# Usage: ci-sanitizer.sh {asan|tsan}
#
# TSan is where nondeterminism surfaces, so the repeat runs live on that leg
# rather than on every one.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly preset="${1:-}"

case "${preset}" in
  asan | tsan) ;;
  *)
    echo "Usage: $0 {asan|tsan}" >&2
    exit 2
    ;;
esac

cd "${root_dir}"

ctest_args=(--test-dir "build/${preset}" --output-on-failure)
if [[ "${preset}" == "tsan" ]]; then
  ctest_args+=(--repeat until-fail:3)
fi

cmake --preset "${preset}"
cmake --build "build/${preset}"
ctest "${ctest_args[@]}"
