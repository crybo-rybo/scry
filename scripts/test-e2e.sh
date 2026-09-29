#!/usr/bin/env bash

# Builds and runs the public-API local-model smoke against an already running
# OpenAI-compatible server. Set SCRY_LOCAL_MODEL_BASE_URL and
# SCRY_LOCAL_MODEL_MODEL before running; an unreachable server fails on connect.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly root_dir
readonly artifact_dir="${root_dir}/build/e2e-artifacts"

: "${SCRY_LOCAL_MODEL_BASE_URL:?Set SCRY_LOCAL_MODEL_BASE_URL to the server /v1 URL}"
: "${SCRY_LOCAL_MODEL_MODEL:?Set SCRY_LOCAL_MODEL_MODEL to the model name}"

# shellcheck source=scripts/gnu-timeout.sh
source "${root_dir}/scripts/gnu-timeout.sh"

cd "${root_dir}"
cmake --preset dev -DCMAKE_CXX_COMPILER="${CXX:-g++-16}" -DSCRY_BUILD_TESTS=ON
cmake --build build/dev --target scry_local_model_smoke

mkdir -p "${artifact_dir}"
gnu_timeout "${SCRY_LOCAL_MODEL_TIMEOUT_SECONDS:-180}" \
  build/dev/tests/e2e/scry_local_model_smoke \
  2>&1 | tee "${artifact_dir}/local-model-smoke.log"
