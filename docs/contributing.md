# Contributing to Scry

This document gives the build commands, the checks, and the requirements for a
contribution to the current tree.

## Toolchain

To build Scry, you must have GCC 16 or newer with reflection support, and CMake
3.30 or newer. You must also have Ninja, and libcurl 7.84 or newer with the
development headers. CMake 3.30 is the first release that knows the C++26 mode
of GCC (`cxx_std_26`). libcurl is the only library dependency of Scry. The
tests fetch Catch2.

The formatter is clang-format, and the linter is clang-tidy. The scripts use
the versions on your `PATH`. CI uses version 22 of both tools. Version 18 and
older format reflection annotations differently, so use version 22 or newer.

**macOS:**

```sh
brew install gcc cmake ninja clang-format llvm
# Homebrew does not put llvm on PATH. Link only clang-tidy:
ln -s "$(brew --prefix llvm)/bin/clang-tidy" "$(brew --prefix)/bin/clang-tidy"
```

**Linux (Ubuntu 24.04):**

```sh
sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test
sudo apt-get install -y g++-16 libcurl4-openssl-dev ninja-build
# Ubuntu 24.04 packages LLVM 18. Get clang-format-22 and clang-tidy-22 from
# https://apt.llvm.org, as .github/workflows/ci.yml does, and set CLANG_FORMAT
# and CLANG_TIDY to them.
# Ubuntu 24.04 packages CMake 3.28. If necessary, install a newer CMake with pip.
```

The presets select `g++-16`. If your compiler has a different name or path, set
`CXX` for the scripts, or use `-DCMAKE_CXX_COMPILER=...` with `cmake --preset`.

## Commands

```sh
./scripts/test.sh                  # configure, build, parallel ctest (dev preset)
./scripts/test.sh -R 'runtime\.'   # the script sends extra arguments to ctest
PRESET=asan ./scripts/test.sh      # or PRESET=tsan
./scripts/format.sh --fix          # --check (the default) only verifies
./scripts/lint.sh                  # clang-tidy and the repository rules
./scripts/test-e2e.sh              # live model; see "End-to-end testing"
```

CI (`.github/workflows/ci.yml`) runs these scripts. One job runs the format
check and the lint. A second job runs `test.sh` on Linux with the `dev`, `asan`,
and `tsan` presets. Before you open a pull request, run the format check, the
lint, and the tests. GCC supplies no thread-sanitizer runtime on Apple Silicon.
Thus, `tsan` runs only on Linux.

The build directories are in `build/<preset>`. They use Ninja and export
`compile_commands.json`. The `.clangd` file points editors to `build/dev`.

| Preset | For |
|---|---|
| `dev` | Debug. For the usual edit-build-test loop. |
| `asan` | Debug, plus ASan and UBSan without recovery. |
| `tsan` | Debug, plus TSan to find data races. |

| Option | Default | Purpose |
|---|---|---|
| `SCRY_BUILD_TESTS` | On at top level | Build the tests |
| `SCRY_BUILD_EXAMPLES` | On at top level | Compile the programs under `examples/` |
| `SCRY_BUILD_TESTING_SUPPORT` | On | Build and install `scry::testing` |
| `SCRY_WARNINGS_AS_ERRORS` | On at top level | Treat project warnings as errors |
| `SCRY_SANITIZER` | `none` | Select `none`, `address-undefined`, or `thread` |

## Tests

The build registers each Catch2 suite with ctest under a prefix for that suite:
`kernel.`, `runtime.`, `machine.`, `protocol.`, `provider.`, `transport.`,
`integration.`, `reflection.`, or `testing.`. `public-api-contract` is a plain
executable test. The build also compiles each public header alone. The
`reflection.compile-fail.*` tests check the diagnostics for incorrect use of
the reflected API.

```sh
ctest --test-dir build/dev -R 'runtime\.'                     # one suite
ctest --test-dir build/dev -R 'event queue coalesces'         # one case by name
```

`tests/fuzz/corpus/` has seed corpora for six `LLVMFuzzerTestOneInput`
harnesses: the SSE parser, the transport response policy, the JSON layer, the
two provider stream decoders, and conversation persistence.
`tests/fuzz/replay_main.cpp` replays each corpus one time as an ordinary
`*-fuzz-replay` test. The `asan` preset replays them with ASan and UBSan. No
build runs a coverage-guided search. Golden fixtures also lock the acceptance
boundary and the canonical bytes of the JSON layer. For more information, see
[`tests/fixtures/json/`](../tests/fixtures/json/README.md).

- **Test behavior at the seams, not the implementation inside them.** Tests
  target the machine, adapter, and transport interfaces. If a refactor of the
  internals breaks a test, that test was coupled to the wrong thing.
- **Use fakes, not mocks.** A hand-written fake transport with scriptable
  responses is better than the expectations of a mock framework. Fakes continue
  to work after refactors, and you can read them as documentation.
- **Tests must always be deterministic.** Do not use real sleeps, wall-clock
  time, or the network in unit tests. Time is an injected event, so a fake
  clock can test backoff to the millisecond. Transport and integration tests
  use local loopback HTTP/TLS servers.
- **Write a test for each bug before you write the fix.** Commit it with the
  fix, and keep it permanently.

## End-to-end testing

Start a model server and load a model. Then run the live smoke:

```sh
SCRY_E2E_BASE_URL=http://127.0.0.1:11434/v1 \
SCRY_E2E_MODEL=qwen3:8b \
./scripts/test-e2e.sh
```

This command builds and runs `scry_e2e_smoke` from `tests/e2e/`. Through the
public API, the smoke runs a typed `ask<T>()` and a streamed turn with one
reflected tool round. Then it restores the conversation from `to_json()` and
sends a follow-up turn. Thus the server must accept a history with a tool call
and its result.

`SCRY_E2E_DIALECT` selects `openai` (the default) or `anthropic`. For
Anthropic, set the origin as the URL, for example `https://api.anthropic.com`,
and set `SCRY_E2E_API_KEY`. The smoke stops immediately if the key is missing.
For an OpenAI-compatible server, the key is optional. ctest and CI do not run
the smoke.

## Testing downstream with `scry::testing`

`scry::testing` is an optional package component. It gives a consumer a
scripted HTTP server on 127.0.0.1. The test sets `Config::base_url` to the URL
of the server and creates the Harness with `Harness::create`. Thus the test
runs all of Scry, libcurl included, and needs no model.

```cmake
find_package(scry CONFIG REQUIRED COMPONENTS testing)
target_link_libraries(my_tests PRIVATE scry::scry scry::testing)
```

```cpp
#include <scry/scry.hpp>
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>

auto server = scry::testing::ScriptedServer::create();
server->enqueue({.body_chunks = {scry::testing::anthropic_text_stream("hi")}});
auto config = my_config();
config.base_url = server->url();
auto harness = scry::Harness::create(config);
```

For a fast scripted retry, set `config.retry.jitter_ratio` to 0 and set
millisecond backoffs. For a fast cancellation, set a low
`config.timeouts.shutdown`.

`examples/testing_scripted.cpp` is a complete test in this form, without a test
framework. `tests/testing/scripted_server_tests.cpp` is the equivalent test
with Catch2.

## Rules

- Top-level builds enable `-Wall -Wextra -Wconversion -Wshadow -Werror`.
- The kernel (`src/kernel/`) is C++23 without reflection, so clang-tidy can
  analyze it. The remaining code in `src/` is C++26, and only GCC compiles it.
  The kernel compiles with `-Werror=c++26-extensions`, and
  `src/kernel/kernel.hpp` rejects a build that enables reflection. `lint.sh`
  makes sure that the kernel includes only kernel headers and
  `<scry/{config,error,json,turn_id,unique_function}.hpp>`.
- clang-tidy uses the checked-in `.clang-tidy`. The cognitive complexity must
  not be more than 25.
- A `// TODO` must link an issue or a URL.

## Pull requests

The project is trunk-based. It uses short-lived branches, squash merges, and
conventional-commit messages. A change lands with tests. A bug fix lands with
its regression test. A public API change lands with an example that compiles.
A behavior change lands with an update to `docs/architecture.md`.

## Releases

1. Increase the version in `project(VERSION ...)` in `CMakeLists.txt`. This is
   the version source of truth. CMake generates `<scry/version.hpp>` from it.
2. In `README.md`, update the `find_package` version and the FetchContent
   `GIT_TAG`. In `tests/public_api_contract.cpp`, update the version
   assertions.
3. Write `docs/releases/vX.Y.Z.md`.
4. Push the tag and publish the release:
   `git tag vX.Y.Z && git push origin vX.Y.Z && gh release create vX.Y.Z --notes-file docs/releases/vX.Y.Z.md`.
