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

ci:
    ./scripts/preflight.sh

docs:
    ./scripts/ci-docs.sh

tidy:
    cmake --preset ci --fresh -B build/tidy -DCMAKE_CXX_COMPILER=clang++ -DSCRY_CLANG_TOOLING=ON -DSCRY_ENABLE_CLANG_TIDY=ON -DSCRY_ENABLE_FORMAT_CHECK=OFF
    cmake --build build/tidy

asan:
    cmake --preset asan
    cmake --build build/asan
    ctest --test-dir build/asan --output-on-failure

tsan:
    cmake --preset tsan
    cmake --build build/tsan
    ctest --test-dir build/tsan --output-on-failure --repeat until-fail:3

nightly-local-model:
    ./scripts/ci-local-model.sh

showcase:
    ./scripts/ci-showcase.sh
