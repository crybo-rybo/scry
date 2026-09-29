#!/usr/bin/env bash

# End-to-end tests of the llamad dialect against a real daemon and model.
#
#   ./scripts/e2e-llamad.sh <llamad checkout> <model.gguf> [Catch2 arguments]
#
# The llamad checkout must be built. Its daemon is build-gpu/llamad, else
# build-cpu/llamad, or $LLAMAD_BINARY. Scry compiles that checkout's
# llamad.proto, so the two sides speak the same contract, and on macOS takes
# gRPC from its build-deps/prefix (scripts/build-deps-macos.sh there).
#
# Builds Scry with SCRY_WITH_LLAMAD=ON in build/e2e-llamad and runs
# tests/e2e/llamad_e2e_tests.cpp, which starts, restarts, and kills its own
# daemon on a private socket. The daemon's log and a JUnit report go to
# build/e2e-llamad-artifacts. Extra arguments go to the test binary, for example
# '~[tools]' to leave out the cases that need a model able to call tools, or
# '[lifecycle]' to run only the daemon restart and kill cases.

set -euo pipefail

if [[ $# -lt 2 ]]; then
  sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly llamad_dir="$(cd "$1" && pwd)"
readonly model="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
shift 2
readonly build_dir="${root_dir}/build/e2e-llamad"
readonly artifact_dir="${root_dir}/build/e2e-llamad-artifacts"

binary="${LLAMAD_BINARY:-}"
if [[ -z "${binary}" ]]; then
  for candidate in build-gpu build-cpu; do
    if [[ -x "${llamad_dir}/${candidate}/llamad" ]]; then
      binary="${llamad_dir}/${candidate}/llamad"
      break
    fi
  done
fi
if [[ ! -x "${binary}" ]]; then
  echo "No llamad daemon under ${llamad_dir}/build-{gpu,cpu}; build it or set LLAMAD_BINARY" >&2
  exit 2
fi
if [[ ! -f "${model}" ]]; then
  echo "Model not found: ${model}" >&2
  exit 2
fi

configure_args=(-DSCRY_WITH_LLAMAD=ON "-DSCRY_LLAMAD_SOURCE_DIR=${llamad_dir}")
if [[ -d "${llamad_dir}/build-deps/prefix" ]]; then
  configure_args+=("-DCMAKE_PREFIX_PATH=${llamad_dir}/build-deps/prefix")
fi

cd "${root_dir}"
cmake --preset ci -B "${build_dir}" "${configure_args[@]}"
cmake --build "${build_dir}" --target scry_llamad_e2e_tests

mkdir -p "${artifact_dir}"
rm -f "${artifact_dir}/llamad.log"
echo "daemon: ${binary}"
echo "model:  ${model}"
echo "log:    ${artifact_dir}/llamad.log"
SCRY_E2E_LLAMAD_BINARY="${binary}" \
  SCRY_E2E_LLAMAD_MODEL="${model}" \
  SCRY_E2E_LLAMAD_LOG="${artifact_dir}/llamad.log" \
  "${build_dir}/tests/e2e/scry_llamad_e2e_tests" \
  --reporter console::out=-::colour-mode=default \
  --reporter "junit::out=${artifact_dir}/junit.xml" \
  --durations yes \
  "$@"
