#!/usr/bin/env bash

# Builds and runs the public-API local-model smoke against an already running
# OpenAI-compatible server. The binary validates SCRY_LOCAL_MODEL_BASE_URL and
# SCRY_LOCAL_MODEL_MODEL; an unreachable server fails the smoke on connect.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly artifact_dir="${root_dir}/build/nightly-local-model-artifacts"

cd "${root_dir}"
cmake --preset ci
cmake --build build/ci --target scry_local_model_smoke

mkdir -p "${artifact_dir}"
timeout "${SCRY_LOCAL_MODEL_TIMEOUT_SECONDS:-180}" \
  build/ci/tests/nightly/scry_local_model_smoke \
  2>&1 | tee "${artifact_dir}/local-model-smoke.log"
