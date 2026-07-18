set shell := ["bash", "-euo", "pipefail", "-c"]

configure:
    cmake --preset dev

build:
    cmake --build build/dev

test:
    ctest --test-dir build/dev --output-on-failure

format:
    cmake --build build/dev --target format

format-check:
    cmake --build build/dev --target format-check

ci-fast:
    ./scripts/ci-local.sh

quality:
    ./scripts/quality-gate.sh

ci:
    ./scripts/preflight.sh

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

curl:
    cmake --preset curl
    cmake --build build/curl
    ctest --test-dir build/curl --output-on-failure

reflection:
    cmake --preset reflection-gcc16
    cmake --build build/reflection-gcc16
