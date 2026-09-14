set shell := ["bash", "-euo", "pipefail", "-c"]

configure:
    cmake --preset dev

build:
    cmake --build build/dev

test:
    ctest --test-dir build/dev --output-on-failure

format:
    ./scripts/format.sh --fix

format-check:
    ./scripts/format.sh --check

ci-fast:
    ./scripts/ci-local.sh

ci:
    ./scripts/preflight.sh

docs:
    ./scripts/ci-docs.sh

tidy:
    ./scripts/ci-tidy.sh

asan:
    ./scripts/ci-sanitizer.sh asan

tsan:
    ./scripts/ci-sanitizer.sh tsan

fuzz:
    ./scripts/ci-fuzz-replay.sh

nightly-local-model:
    ./scripts/ci-local-model.sh

showcase:
    ./scripts/ci-showcase.sh
