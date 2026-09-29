set shell := ["bash", "-euo", "pipefail", "-c"]

configure:
    cmake --preset dev

build:
    cmake --build build/dev

test:
    ./scripts/test.sh

e2e:
    ./scripts/test-e2e.sh

format:
    ./scripts/format.sh --fix

format-check:
    ./scripts/format.sh --check

ci-fast:
    ./.github/scripts/ci-local.sh

ci:
    ./scripts/ci.sh

docs:
    ./.github/scripts/ci-docs.sh

tidy:
    ./.github/scripts/ci-tidy.sh

asan:
    ./.github/scripts/ci-sanitizer.sh asan

tsan:
    ./.github/scripts/ci-sanitizer.sh tsan

fuzz:
    ./.github/scripts/ci-fuzz-replay.sh

showcase:
    ./.github/scripts/ci-showcase.sh
