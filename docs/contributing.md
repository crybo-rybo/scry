# Contributing to Scry

Build commands, checks, and contribution requirements for the current tree.

## Toolchain

Scry needs GCC 16 or newer (with reflection support), CMake 3.28 or newer,
Ninja, and libcurl 7.84 or newer with development headers. Development presets
also require clang-format; CI uses version 18.

**Linux:**

```sh
sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install -y g++-16 libcurl4-openssl-dev ninja-build \
  cmake clang-format-18 doxygen graphviz
```

**macOS:**

```sh
brew install gcc cmake llvm@18 ninja doxygen graphviz
export PATH="$(brew --prefix llvm@18)/bin:$PATH"
```

Both platforms also need the complexity checker, which CI pins:

```sh
python3 -m pip install --user --break-system-packages lizard==1.24.0
```

Ensure `g++-16 --version` succeeds. When the compiler has another path, pass it
with `-DCMAKE_CXX_COMPILER=...`. Linux installations with only the versioned
formatter can configure with
`-DSCRY_CLANG_FORMAT_EXECUTABLE=clang-format-18`. Doxygen and Graphviz are needed
for the documentation gate; Doxygen 1.9.8 is the minimum.

The optional clang-tidy gate needs Clang and clang-tidy. CI uses version 18;
preflight probes Homebrew's keg-only `llvm@18` before `llvm` when clang-tidy is
absent from `PATH`. Fuzzing needs a Clang installation with libFuzzer; the hosted
fuzz legs use Clang 21.

## Presets

Build directories live under `build/<preset>`, Ninja, with
`compile_commands.json` exported; `.clangd` points at `build/dev`.

| Preset | For |
|---|---|
| `dev` | Debug. The everyday edit-build-test loop. |
| `ci` | RelWithDebInfo. What `ci-local.sh` builds, installs, and audits. |
| `asan` | Debug plus ASan and non-recovering UBSan. |
| `tsan` | Debug plus TSan for race detection. |
| `fuzz` | Clang with `SCRY_CLANG_TOOLING`, libFuzzer, ASan and UBSan. |

Every GCC preset pins `CMAKE_CXX_COMPILER` to `g++-16` through a hidden `gcc`
preset. Override it when your GCC 16 is spelled differently:
`cmake --preset dev -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/g++-16`.
The `fuzz` preset does not select its compiler:

```sh
cmake --preset fuzz -DCMAKE_CXX_COMPILER=clang++-21
cmake --build build/fuzz
ctest --test-dir build/fuzz --output-on-failure
```

Use a compatible local Clang path in place of `clang++-21` as needed. When
changing a compiler in an existing build directory, add `--fresh` to the
configure command so CMake reapplies the preset without stale cache settings.

## Build options

| Option | Default | Purpose |
|---|---|---|
| `SCRY_BUILD_TESTS` | On at top level | Build tests and standalone header checks |
| `SCRY_BUILD_EXAMPLES` | On at top level | Compile `examples/main_loop.cpp` |
| `SCRY_WARNINGS_AS_ERRORS` | On at top level | Treat project warnings as errors |
| `SCRY_ENABLE_FORMAT_CHECK` | Off; on in `dev` and `ci` | Create `format` and `format-check` targets |
| `SCRY_ENABLE_CLANG_TIDY` | Off | Analyze library sources while compiling |
| `SCRY_CLANG_TOOLING` | Off | Build the C++23 implementation with Clang for tooling |
| `SCRY_CLANG_TOOLING_LIBCXX` | Off | Select libc++ in Clang tooling mode |
| `SCRY_BUILD_FUZZERS` | Off | Build libFuzzer targets with Clang |
| `SCRY_SANITIZER` | `none` | Select `none`, `address-undefined`, or `thread` |

Tests, examples, and warnings-as-errors default to off when Scry is embedded.
Clang tooling mode disables examples and ordinary tests; enable fuzzers to
build its test targets. The consumer build always includes reflection in
`scry::scry`.

## The loop

```sh
cmake --preset dev                                # just configure
cmake --build build/dev                           # just build
ctest --test-dir build/dev --output-on-failure    # just test
```

Catch2 suites are registered with ctest under a per-suite prefix (`runtime.`,
`machine.`, `protocol.`, `provider.`, `transport.`, `integration.`,
`reflection.`); `public-api-contract` is a plain executable test.

```sh
ctest --test-dir build/dev -R 'runtime\.'                     # one suite
ctest --test-dir build/dev -R 'event queue coalesces'         # one case by name
./build/dev/tests/scry_runtime_tests "event queue coalesces adjacent deltas"
```

Formatting is clang-format, LLVM base, 88 columns:

```sh
cmake --build build/dev --target format         # just format
cmake --build build/dev --target format-check   # just format-check
```

## Gates

The core, documentation, clang-tidy, sanitizer, fuzz, showcase, and local-model
checks have scripts under `scripts/`. Workflows supply their toolchains and
invoke those scripts. The pinned format check, CodeQL, and release publication
also have workflow-specific steps.

**Per commit** (`.github/workflows/ci.yml`):

| Job | Runs |
|---|---|
| Doxygen API site + clang-format | `./scripts/ci-docs.sh`, then a pinned `clang-format-18` dry run |
| Core, Linux GCC 16 and macOS GCC 16 | `./scripts/ci-local.sh` |
| clang-tidy | `./scripts/ci-tidy.sh` with `SCRY_TIDY_LIBCXX=1`, because Ubuntu 24.04's libstdc++ `<expected>` is newer than clang 18 can parse |
| ASan + UBSan, TSan | `./scripts/ci-sanitizer.sh asan` and `... tsan` |
| Fuzz corpus replay | `./scripts/ci-fuzz-replay.sh` |

**Weekly, Mondays** (`.github/workflows/nightly.yml`): CodeQL; a long fuzz run on
each of the five targets (`./scripts/ci-nightly-fuzz.sh <target>`); and the
showcase gate (`./scripts/ci-showcase.sh`). The end-to-end smoke against a real
local model (`./scripts/ci-local-model.sh`) is `workflow_dispatch` only — it
exercises a live model, so it does not gate pull requests.

**On a tag** (`release.yml`): `check-release-tag.sh`, the core gate, the API
site, and the GitHub release built from the checked-in notes.

Run the whole per-commit ring locally before every pull request:

```sh
./scripts/preflight.sh    # just ci
```

It runs documentation, core, clang-tidy, sanitizers, and fuzz replay, and
continues after failures. Missing documentation, tidy, sanitizer, or fuzz
capabilities are reported as `SKIP` and listed in the closing summary. The core
gate is always attempted: a missing compiler, formatter, or complexity checker
fails that gate. Each sanitizer leg probes its own flag with
`g++-16` first, because GCC ships no thread-sanitizer runtime on Apple Silicon,
so TSan skips there while ASan still runs. `./scripts/ci-local.sh` (`just
ci-fast`) is the faster inner loop: diff check, complexity, unlinked TODOs,
format, build, tests, a staged install, and a downstream `find_package(scry)`
consumer.

The showcase is a standalone project under `extras/showcase/` that the root build
never configures; `./scripts/ci-showcase.sh` (`just showcase`) builds it, runs
the deterministic NPC and fake-panel tests and a real headless ImGui frame, then
audits that nothing it adds reached the installed package.

The five fuzz targets are `sse`, `anthropic`, `openai`, `response_policy`, and
`conversation`. Each replays its checked-in seed corpus per commit, so a target
cannot rot between long runs. For a long local search:

```sh
SCRY_NIGHTLY_FUZZ_SECONDS=1200 ./scripts/ci-nightly-fuzz.sh sse
```

## Testing

- **Test behavior at seams, not implementation inside them.** Tests target the
  machine, adapter, and transport interfaces. If refactoring internals breaks a
  test, the test was coupled to the wrong thing.
- **Fakes over mocks.** A hand-written fake transport with scriptable responses
  beats mock-framework expectations: fakes survive refactors and read as
  documentation. The seams are few and narrow enough to fake properly.
- **Determinism is non-negotiable.** No real sleeps, wall-clock time, or network
  in unit tests; time is an injected event, so a fake clock makes backoff
  testable to the millisecond. A flaky test is fixed or deleted the day it flakes.
- **Every bug becomes a test before it becomes a fix**, usually a machine-level
  event replay, committed with the fix permanently.
- **Choose the relevant seam.** Machine tests cover transitions, adapters cover
  wire mapping, runtime tests cover the pump and handles, and reflection tests
  cover schemas and codecs. Transport and integration tests also use local
  loopback HTTP/TLS servers; the optional local-model smoke uses a live model.

## Mechanical limits

- Top-level builds enable `-Wall -Wextra -Wconversion -Wshadow` and treat
  warnings as errors on GCC and Clang. `SCRY_WARNINGS_AS_ERRORS` controls this.
- lizard: cyclomatic complexity must not exceed 15 and argument count must not
  exceed 6, for C++ in `include src examples tests extras`.
- clang-tidy: cognitive complexity must not exceed 25, with a checked-in check
  list. The tidy script uses `SCRY_CLANG_TOOLING` and analyzes the library
  target; it does not analyze reflection, examples, or ordinary tests.
- `// TODO` must link an issue or a URL. CI rejects any unlinked TODO outright.

## Definition of done

- `./scripts/preflight.sh` ran, and any skipped legs are named.
- Tests are added or updated; a bug fix includes its regression test.
- [`docs/architecture.md`](architecture.md) is updated when behavior changes.
- An example compiles the change when the public API changes.
- A dependency change carries a written justification in the same commit.

## Pull requests

Trunk-based: short-lived branches, squash merge, conventional-commit messages,
`main` always green and always releasable. The pull-request template carries the
preflight, test coverage, and documentation checkboxes.

## Releases

1. Bump `project(VERSION ...)` in `CMakeLists.txt`. That is the version source of
   truth; `<scry/version.hpp>` is generated from it.
2. Update both the `find_package` version and FetchContent `GIT_TAG` in
   `README.md`, the package version in `tests/package_consumer/CMakeLists.txt`,
   and the version assertions in `tests/public_api_contract.cpp`.
3. Write `docs/releases/vX.Y.Z.md`.
4. Check the tag first: `./scripts/check-release-tag.sh vX.Y.Z`.
5. Push the tag. The release workflow re-runs the core gate against the tagged
   tree, builds the API site, and publishes the release from those notes.
