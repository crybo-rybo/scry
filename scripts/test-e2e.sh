#!/usr/bin/env bash

# Build and run the public-API smoke against a live model server.
# Set SCRY_E2E_BASE_URL and SCRY_E2E_MODEL first. SCRY_E2E_DIALECT selects
# openai (the default) or anthropic. SCRY_E2E_API_KEY is optional for openai and
# required for anthropic, whose URL is the origin, e.g. https://api.anthropic.com.
# The smoke runs a typed ask<T>(), a streamed turn with one reflected tool round,
# then restores the conversation from to_json() and sends a follow-up turn, so the
# server must accept a history that holds a tool call and its result. ctest and
# CI do not run it.

set -euo pipefail

: "${SCRY_E2E_BASE_URL:?Set SCRY_E2E_BASE_URL to the server URL}"
: "${SCRY_E2E_MODEL:?Set SCRY_E2E_MODEL to the model name}"

cd "$(dirname "${BASH_SOURCE[0]}")/.."
cmake --preset dev -DCMAKE_CXX_COMPILER="${CXX:-g++-16}"
cmake --build build/dev --target scry_e2e_smoke
build/dev/tests/e2e/scry_e2e_smoke
