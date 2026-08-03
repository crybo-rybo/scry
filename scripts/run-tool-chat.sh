#!/usr/bin/env bash

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/dev-logging"
readonly executable="${build_dir}/examples/scry_tool_chat"
readonly log_file="${SCRY_LOG_FILE:-${build_dir}/tool-chat.log}"

export SCRY_LOCAL_MODEL_BASE_URL="${SCRY_LOCAL_MODEL_BASE_URL:-http://127.0.0.1:11434/v1}"
export SCRY_LOCAL_MODEL_MODEL="${SCRY_LOCAL_MODEL_MODEL:-qwen3:8b}"
export SCRY_LOG_FILE="${log_file}"

cd "${root_dir}"
cmake --preset dev-logging
cmake --build "${build_dir}" --target scry_tool_chat

echo "Starting Scry tool chat with ${SCRY_LOCAL_MODEL_MODEL}."
echo "Diagnostic log: ${SCRY_LOG_FILE}"
exec "${executable}"
