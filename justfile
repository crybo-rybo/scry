set shell := ["bash", "-euo", "pipefail", "-c"]

configure:
    cmake --preset dev

build:
    cmake --build build/dev

test:
    ctest --test-dir build/dev --output-on-failure

profile *args:
    ./scripts/perf-run.sh --mode full {{ args }}

profile-smoke *args:
    ./scripts/perf-run.sh --mode smoke {{ args }}

profile-dry *args:
    ./scripts/perf-run.sh --mode dry {{ args }}

profile-compare base head output="build/profile-comparison" *args:
    python3 scripts/perf-compare.py compare --base "{{ base }}" --head "{{ head }}" --output-dir "{{ output }}" {{ args }}

profile-pair base head output="build/profile-pair" *args:
    ./scripts/perf-pair.sh --base-dir "{{ base }}" --head-dir "{{ head }}" --output "{{ output }}" {{ args }}

format:
    cmake --build build/dev --target format

format-check:
    cmake --build build/dev --target format-check

ci-fast:
    ./scripts/ci-local.sh

ci:
    ./scripts/preflight.sh

docs:
    ./scripts/ci-docs.sh

tidy:
    cmake --preset ci -B build/tidy -DSCRY_ENABLE_CLANG_TIDY=ON -DSCRY_ENABLE_FORMAT_CHECK=OFF
    cmake --build build/tidy

asan:
    cmake --preset asan
    cmake --build build/asan
    ctest --test-dir build/asan --output-on-failure

tsan:
    cmake --preset tsan
    cmake --build build/tsan
    ctest --test-dir build/tsan --output-on-failure

reflection:
    ./scripts/ci-reflection.sh

nightly-local-model:
    ./scripts/ci-local-model.sh

showcase:
    ./scripts/ci-showcase.sh
