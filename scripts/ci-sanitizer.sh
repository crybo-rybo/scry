#!/usr/bin/env bash

# One sanitizer leg, run identically by preflight and by hosted CI.
#
# Usage: ci-sanitizer.sh {asan|tsan}
#
# TSan is where nondeterminism surfaces, so the repeat runs live on that leg
# (QA-008) rather than on every one.

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

cmake --preset "${preset}"
cmake --build "build/${preset}"
if [[ "${preset}" == "tsan" ]]; then
  ctest --test-dir build/tsan --output-on-failure --repeat until-fail:3
else
  ctest --test-dir build/asan --output-on-failure
fi
