#!/usr/bin/env bash

# Build and run the public-API smoke against a running OpenAI-compatible server.
# Set SCRY_LOCAL_MODEL_BASE_URL and SCRY_LOCAL_MODEL_MODEL first.

set -euo pipefail

: "${SCRY_LOCAL_MODEL_BASE_URL:?Set SCRY_LOCAL_MODEL_BASE_URL to the server /v1 URL}"
: "${SCRY_LOCAL_MODEL_MODEL:?Set SCRY_LOCAL_MODEL_MODEL to the model name}"

cd "$(dirname "${BASH_SOURCE[0]}")/.."
cmake --preset dev -DCMAKE_CXX_COMPILER="${CXX:-g++-16}"
cmake --build build/dev --target scry_local_model_smoke
build/dev/tests/e2e/scry_local_model_smoke
