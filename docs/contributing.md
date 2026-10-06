# Contributing to Scry

This document gives the build commands, the checks, and the requirements for a
contribution to the current tree.

## Toolchain

To build Scry, you must have GCC 16 or newer with reflection support, and CMake
3.30 or newer. You must also have Ninja, and libcurl 7.84 or newer with the
development headers. CMake 3.30 is the first release that knows the C++26 mode
of GCC (`cxx_std_26`). libcurl is the only library dependency of Scry. The
tests fetch Catch2. The formatter is clang-format, and CI uses version 18. The
formatter runs independently of CMake. It does not need a compiler or the
dependencies that the build fetches.

**Linux:**

```sh
sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install -y g++-16 libcurl4-openssl-dev ninja-build \
  clang-format-18 doxygen graphviz
# Ubuntu 24.04 packages CMake 3.28, which has no C++26 mode for GCC.
# pip installs cmake into ~/.local/bin, which must precede /usr/bin on PATH.
python3 -m pip install --user --break-system-packages 'cmake>=3.30'
```

**macOS:**

```sh
brew install gcc cmake llvm@21 ninja doxygen graphviz
export PATH="$(brew --prefix llvm@21)/bin:$PATH"
```

On both platforms, also install the complexity checker at the version that CI
pins:

```sh
python3 -m pip install --user --break-system-packages lizard==1.24.0
```

Make sure that `g++-16 --version` succeeds. If the compiler has a different
path, set that path with `-DCMAKE_CXX_COMPILER=...`. To select a formatter with
a version in its name, use
`CLANG_FORMAT=clang-format-18 ./scripts/format.sh --check`. Doxygen and Graphviz
are necessary for the documentation gate. The minimum Doxygen version is 1.9.8.

The optional clang-tidy gate and the libFuzzer targets of the kernel need Clang
21 with clang-tidy and libFuzzer. On macOS, the `llvm@21` export above gives
this version. On Linux, use the apt.llvm.org packages that
`.github/actions/fuzz-toolchain` installs.

## Presets

The build directories are in `build/<preset>`. They use Ninja and export
`compile_commands.json`. The `.clangd` file points to `build/dev`.

| Preset | For |
|---|---|
| `dev` | Debug. For the usual edit-build-test loop. |
| `ci` | RelWithDebInfo. `.github/scripts/ci-local.sh` builds, installs, and audits this preset. |
| `asan` | Debug, plus ASan and UBSan without recovery. |
| `tsan` | Debug, plus TSan to find data races. |
| `fuzz` | Clang with `SCRY_CLANG_TOOLING`: the libFuzzer targets of the kernel, ASan and UBSan. |

Each GCC preset pins `CMAKE_CXX_COMPILER` to `g++-16` through a hidden `gcc`
preset. If your GCC 16 has a different name or path, override this value:
`cmake --preset dev -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/g++-16`.
The `fuzz` preset does not select its compiler. Select it when you configure:

```sh
cmake --preset fuzz -DCMAKE_CXX_COMPILER=clang++-21
cmake --build build/fuzz
ctest --test-dir build/fuzz --output-on-failure
```

If necessary, replace `clang++-21` with the path of a compatible local Clang. If
you change the compiler in an existing build directory, add `--fresh` to the
configure command. Then CMake applies the preset again without old cache
settings.

The root `CMakeLists.txt` defines the kernel sources, the library sources, the
dependencies, and the package. The kernel is the `scry_kernel` object library.
The build compiles it as C++23 and puts it in the `scry` archive.
`cmake/ScryCompilerChecks.cmake` has the compiler compatibility checks. The
reflection probe is in `cmake/probes/reflection.cpp`.
`cmake/ScryDeveloperTools.cmake` has the warning flags, the sanitizers,
clang-tidy, and the public-header audit. `cmake/CheckKernelBoundary.cmake` has
the kernel include audit. `cmake/ScryFuzz.cmake` registers the fuzz targets.
The standalone header checks share one build target. Each header has its own
translation unit.

## Build options

| Option | Default | Purpose |
|---|---|---|
| `SCRY_BUILD_TESTS` | On at top level | Build tests and standalone header checks |
| `SCRY_BUILD_EXAMPLES` | On at top level | Compile the programs under `examples/` |
| `SCRY_BUILD_TESTING_SUPPORT` | On | Build and install `scry::testing` |
| `SCRY_WARNINGS_AS_ERRORS` | On at top level | Treat project warnings as errors |
| `SCRY_ENABLE_CLANG_TIDY` | Off | Analyze the kernel sources during compilation |
| `SCRY_CLANG_TOOLING` | Off | Build only the C++23 kernel with Clang for tooling |
| `SCRY_BUILD_FUZZERS` | Off | Build the libFuzzer targets of the kernel with Clang |
| `SCRY_SANITIZER` | `none` | Select `none`, `address-undefined`, or `thread` |

When another project embeds Scry, tests, examples, and warnings-as-errors are
off by default. Clang tooling mode builds only the kernel. It does not build
`scry::scry`, `scry::testing`, examples, or ordinary tests. Fuzzers must have
that mode. The build registers them separately under `tests/fuzz/`, even if
`SCRY_BUILD_TESTS=OFF`. The consumer build always includes reflection in
`scry::scry`.

## The loop

One command builds and runs the unit and integration tests. These tests do not
need a live model. The transport suites use local loopback servers.

```sh
./scripts/test.sh                           # just test
./scripts/test.sh -R 'runtime\.'            # filter ctest cases
```

The script configures and builds the `dev` preset. Then it runs ctest. If the
compiler has a different name, set `CXX=/path/to/g++-16`. You can also run each
step separately:

```sh
cmake --preset dev                                # just configure
cmake --build build/dev                           # just build
ctest --test-dir build/dev --output-on-failure    # just test
```

The build registers each Catch2 suite with ctest under a prefix for that suite:
`kernel.`, `runtime.`, `machine.`, `protocol.`, `provider.`, `transport.`,
`integration.`, `reflection.`, or `testing.`. `public-api-contract` is a plain
executable test. `kernel.include-boundary` runs
`cmake/CheckKernelBoundary.cmake`.

```sh
ctest --test-dir build/dev -R 'runtime\.'                     # one suite
ctest --test-dir build/dev -R 'event queue coalesces'         # one case by name
./build/dev/tests/scry_runtime_tests "event queue coalesces adjacent deltas"
```

The format style is clang-format with the LLVM base style and 88 columns. The
shared script checks tracked and untracked C++ sources, but not ignored files:

```sh
./scripts/format.sh --fix      # just format
./scripts/format.sh --check    # just format-check
```

## Gates

The CI helpers are in `.github/scripts/`. The workflows supply the toolchains
for these helpers and run them. `scripts/ci.sh` runs them locally. The user
commands are in `scripts/`. These include the format script and the two test
entry points.

**Per commit** (`.github/workflows/ci.yml`):

| Job | Runs |
|---|---|
| Doxygen API site + clang-format | `./.github/scripts/ci-docs.sh`, then `CLANG_FORMAT=clang-format-18 ./scripts/format.sh --check` |
| Core, Linux GCC 16 and macOS GCC 16 | `./.github/scripts/ci-local.sh` |
| clang-tidy | `./.github/scripts/ci-tidy.sh` with Clang 21 |
| ASan + UBSan, TSan | `./.github/scripts/ci-sanitizer.sh asan` and `... tsan` |
| Kernel fuzz corpus replay | `./.github/scripts/ci-fuzz-replay.sh` |

**On a tag** (`release.yml`): This workflow runs `check-release-tag.sh` and the
core gate. It also builds the API site. Then it builds the GitHub release from
the checked-in notes.

Before each pull request, run the CI checks and the showcase build locally:

```sh
./scripts/ci.sh    # just ci
```

It runs the documentation, format, core, clang-tidy, sanitizer, and kernel fuzz
replay gates. It also builds the showcase. It continues after a failure. If the
host cannot run the documentation, tidy, sanitizer, or fuzz gate, the script
reports that gate as `SKIP`. The summary at the end lists each `SKIP` gate
again.

The script always tries the format and core gates. If the formatter is missing,
the format gate fails. If the compiler or the complexity checker is missing, the
core gate fails. Each sanitizer leg first probes its own flag with `g++-16`. The
reason is that GCC supplies no thread-sanitizer runtime on Apple Silicon. Thus,
on Apple Silicon, TSan skips but ASan runs.

`./.github/scripts/ci-local.sh` (`just ci-fast`) is the faster inner loop. It
does a whitespace check of the branch against `origin/main`, the complexity
check, and the check for unlinked TODOs. It also does the build, the tests, a
staged install, and a downstream `find_package(scry)` consumer.

The showcase is a standalone project under `extras/showcase/`. The root build
never configures it. `./.github/scripts/ci-showcase.sh` (`just showcase`) only
builds it.

There are six fuzz targets. Each target has a checked-in seed corpus under
`tests/fuzz/corpus/`, and it replays that corpus on each commit.

| Target | Tests | Build type | Per-commit test |
|---|---|---|---|
| `sse` | Kernel SSE parser | libFuzzer, `fuzz` preset | `protocol.sse-fuzz` |
| `response_policy` | Kernel transport response policy | libFuzzer, `fuzz` preset | `transport.response_policy-fuzz` |
| `json` | Kernel JSON layer: parse and validate give the same result, and canonical text round-trips and is idempotent | libFuzzer, `fuzz` preset; also GCC corpus replay | `kernel.json-fuzz`, `kernel.json-fuzz-replay` |
| `anthropic` | Anthropic stream decoder | GCC corpus replay | `provider.anthropic-fuzz-replay` |
| `openai` | OpenAI-compatible stream decoder | GCC corpus replay | `provider.openai-fuzz-replay` |
| `conversation` | Conversation persistence | GCC corpus replay | `runtime.conversation-fuzz-replay` |

`.github/scripts/ci-fuzz-replay.sh` runs the kernel targets. The other three
targets link the full library, and only GCC compiles that library. Thus, the
ordinary test build links each of these targets to `tests/fuzz/replay_main.cpp`
instead of libFuzzer. Each GCC leg replays their corpora. The `asan` preset
replays them with ASan and UBSan. But no gate runs a coverage-guided search on
these targets. Golden fixtures also lock the acceptance boundary and the
canonical bytes of the JSON layer. The `kernel.` tests check these fixtures.
For more information, see
[`tests/fixtures/json/`](../tests/fixtures/json/README.md).

## End-to-end testing

Start an OpenAI-compatible server and load a model. Then, if necessary, run the
live-model smoke:

```sh
SCRY_LOCAL_MODEL_BASE_URL=http://127.0.0.1:11434/v1 \
SCRY_LOCAL_MODEL_MODEL=qwen3:8b \
./scripts/test-e2e.sh                        # just e2e with the same environment
```

This command builds `scry_local_model_smoke` from `tests/e2e/`. Through the
public API, the smoke checks a full chat and a required tool round. If the
server needs authentication, set `SCRY_LOCAL_MODEL_API_KEY`. To change the
180-second timeout, set `SCRY_LOCAL_MODEL_TIMEOUT_SECONDS`. The script must have
GNU timeout. On macOS, install coreutils. The script writes its log to
`build/e2e-artifacts/local-model-smoke.log`.

Default builds and the ctest registration do not include the smoke executable.
Only `scripts/test-e2e.sh` builds and runs it explicitly. The CI checks do not
use a live model.

## Testing

- **Test behavior at the seams, not the implementation inside them.** Tests
  target the machine, adapter, and transport interfaces. If a refactor of the
  internals breaks a test, that test was coupled to the wrong thing.
- **Use fakes, not mocks.** A hand-written fake transport with scriptable
  responses is better than the expectations of a mock framework. Fakes continue
  to work after refactors, and you can read them as documentation. The seams
  are few and narrow, so you can fake them correctly.
- **Tests must always be deterministic.** Do not use real sleeps, wall-clock
  time, or the network in unit tests. Time is an injected event, so a fake
  clock can test backoff to the millisecond. If a test is flaky, fix it or
  delete it on the day that it flakes.
- **Write a test for each bug before you write the fix.** This test is usually
  an event replay at the machine level. Commit it with the fix, and keep it
  permanently.
- **Choose the correct seam.** Machine tests cover transitions. Adapter tests
  cover wire mapping. Runtime tests cover the pump and the handles. Reflection
  tests cover schemas and codecs. Transport and integration tests also use
  local loopback HTTP/TLS servers. The optional local-model smoke uses a live
  model.

## Testing downstream with `scry::testing`

`scry::testing` is an optional package component. It gives a consumer the same
scripted-transport seam that the Scry test suites use. It replaces only the HTTP
transfer.

```cmake
find_package(scry CONFIG REQUIRED COMPONENTS testing)
target_link_libraries(my_tests PRIVATE scry::scry scry::testing)
```

```cpp
#include <scry/scry.hpp>
#include <scry/testing/scripted_transport.hpp>
#include <scry/testing/streams.hpp>

scry::testing::ScriptedTransport transport;
transport.enqueue({.body_chunks = {scry::testing::anthropic_text_stream("hi")}});
auto harness = scry::testing::create_harness(my_config(), transport);
```

`<scry/testing/scripted_transport.hpp>` and `<scry/testing/streams.hpp>` document
the scripted statuses, the failures, the held transfers, and the stream builders.
`examples/testing_scripted.cpp` is a complete test in this form, without a test
framework. `tests/testing/scripted_transport_tests.cpp` is the equivalent test
with Catch2.

## Mechanical limits

- Top-level builds enable `-Wall -Wextra -Wconversion -Wshadow` and treat
  warnings as errors on GCC and Clang. `SCRY_WARNINGS_AS_ERRORS` controls this.
- lizard: For C++ in `include src testing examples tests extras`, the
  cyclomatic complexity must not be more than 15. The argument count must not
  be more than 6.
- clang-tidy: The cognitive complexity must not be more than 25. The repository
  has the list of checks. The tidy script uses `SCRY_CLANG_TOOLING` and
  analyzes the kernel (`scry_kernel`). It does not analyze the remaining code
  in `src/`, examples, or tests.
- The kernel is C++23 without reflection. Its sources compile without
  `-freflection`. Under GCC, they also compile with `-Werror=c++26-extensions`.
  Thus, if a file that they include has a reflection operator or annotation,
  the build fails. `src/kernel/kernel.hpp` rejects a build that enables
  reflection. `kernel.include-boundary` fails if kernel code includes a public
  header that is not on the allowlist. It also fails if kernel code includes a
  `src/` header that is not in the kernel.
- A `// TODO` must link an issue or a URL. CI always rejects a TODO without a
  link.

## Definition of done

A change is done when you complete these steps:

- Run `./scripts/ci.sh`, and name each leg that it skipped.
- Add or update tests. For a bug fix, include its regression test.
- If the behavior changes, update [`docs/architecture.md`](architecture.md).
- If the public API changes, make sure that an example compiles the change.
- If you change a dependency, write a justification in the same commit.

## Pull requests

The project is trunk-based. It uses short-lived branches, squash merges, and
conventional-commit messages. `main` is always green and always releasable. The
pull request template has three checkboxes: preflight, test coverage, and
documentation.

## Releases

1. Increase the version in `project(VERSION ...)` in `CMakeLists.txt`. This is
   the version source of truth. CMake generates `<scry/version.hpp>` from it.
2. In `README.md`, update the `find_package` version and the FetchContent
   `GIT_TAG`. In `tests/package_consumer/CMakeLists.txt`, update the package
   version. In `tests/public_api_contract.cpp`, update the version assertions.
3. Write `docs/releases/vX.Y.Z.md`.
4. Before you push the tag, check it: `./scripts/check-release-tag.sh vX.Y.Z`.
5. Push the tag. The release workflow runs the core gate again on the tagged
   tree. Then it builds the API site and publishes the release from those notes.
