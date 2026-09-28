#!/usr/bin/env bash

# The llamad leg, run identically by preflight and by hosted CI.
#
# Builds Scry with SCRY_WITH_LLAMAD=ON (the ci preset, in build/llamad), runs
# the backend suite - the gRPC backend against a scripted fake daemon on a
# private Unix socket - and the runtime suite, then installs the result and
# builds the downstream consumer with SCRY_CONSUMER_LLAMAD=ON, which proves the
# installed package finds gRPC and Protobuf and accepts the llamad dialect. No
# real daemon or model is involved.
#
# Needs gRPC and Protobuf development packages; on Ubuntu 24.04:
#   libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc
# Extra arguments are forwarded to the configure step, for example
# -DSCRY_LLAMAD_SOURCE_DIR=/path/to/llamad to use a local checkout.

set -euo pipefail

readonly root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly build_dir="${root_dir}/build/llamad"
readonly stage_dir="${root_dir}/build/llamad-stage"
readonly consumer_dir="${root_dir}/build/llamad-package-consumer"

cd "${root_dir}"

cmake --preset ci -B "${build_dir}" -DSCRY_WITH_LLAMAD=ON "$@"
cmake --build "${build_dir}"
ctest \
  --test-dir "${build_dir}" \
  --output-on-failure \
  -R '^(backend|runtime)\.'

cmake -E remove_directory "${stage_dir}"
cmake --install "${build_dir}" --prefix "${stage_dir}"

cmake -E remove_directory "${consumer_dir}"
cmake \
  -S tests/package_consumer \
  -B "${consumer_dir}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="${CXX:-g++-16}" \
  -DCMAKE_PREFIX_PATH="${stage_dir}" \
  -DSCRY_CONSUMER_LLAMAD=ON
cmake --build "${consumer_dir}"
"${consumer_dir}/scry_package_consumer"
