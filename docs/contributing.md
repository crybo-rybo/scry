# Contributing to Scry

## Toolchain

Scry needs GCC 16 or newer, CMake 3.30 or newer, Ninja, and libcurl 7.84 or
newer with development headers. Tests fetch Catch2. Formatting uses
clang-format 18 (newer versions lay out reflection annotations differently);
linting uses clang-tidy 21 or newer.

**macOS:**

```sh
brew install gcc cmake ninja llvm llvm@18
export CLANG_FORMAT="$(brew --prefix llvm@18)/bin/clang-format"
export CLANG_TIDY="$(brew --prefix llvm)/bin/clang-tidy"
```

**Linux (Ubuntu 24.04):**

```sh
sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test
sudo apt-get install -y g++-16 libcurl4-openssl-dev ninja-build clang-format-18
# clang-tidy 21 comes from https://apt.llvm.org; see .github/workflows/ci.yml.
# Ubuntu 24.04 packages CMake 3.28; install a newer one with pip if needed.
```

The presets select `g++-16`; when yours has another name or path, set `CXX`
for the scripts or pass `-DCMAKE_CXX_COMPILER=...` to `cmake --preset`.

## Commands

```sh
./scripts/test.sh                  # configure, build, and run ctest (dev preset)
./scripts/test.sh -R 'runtime\.'   # extra arguments go to ctest
PRESET=asan ./scripts/test.sh      # or PRESET=tsan
./scripts/format.sh --fix          # --check (the default) to verify only
./scripts/lint.sh                  # clang-tidy and repository rules
./scripts/test-e2e.sh              # live model; see below
```

CI (`.github/workflows/ci.yml`) runs exactly these: format and lint, then
`test.sh` under the `dev`, `asan`, and `tsan` presets on Linux. Run all three
before opening a pull request. GCC ships no thread sanitizer runtime on Apple
Silicon, so `tsan` runs only on Linux.

Presets build into `build/<preset>` and export `compile_commands.json`;
`.clangd` points editors at `build/dev`.

| Preset | For |
|---|---|
| `dev` | Debug. The everyday loop. |
| `asan` | Debug plus ASan and non-recovering UBSan. |
| `tsan` | Debug plus TSan. |

| Option | Default | Purpose |
|---|---|---|
| `SCRY_BUILD_TESTS` | On at top level | Build the tests |
| `SCRY_BUILD_EXAMPLES` | On at top level | Compile `examples/` |
| `SCRY_BUILD_TESTING_SUPPORT` | On | Build and install `scry::testing` |
| `SCRY_WARNINGS_AS_ERRORS` | On at top level | `-Werror` |
| `SCRY_SANITIZER` | `none` | `none`, `address-undefined`, or `thread` |

## Tests

Catch2 suites are registered with ctest under a per-suite prefix (`kernel.`,
`runtime.`, `machine.`, `protocol.`, `provider.`, `transport.`, `integration.`,
`reflection.`, `testing.`). Every public header also compiles on its own as part
of the build, and `reflection.compile-fail.*` checks the diagnostics a misuse of
the reflected API produces.

```sh
ctest --test-dir build/dev -R 'runtime\.'                  # one suite
ctest --test-dir build/dev -R 'event queue coalesces'      # one case by name
```

`tests/fuzz/corpus/` holds seed corpora for six `LLVMFuzzerTestOneInput`
harnesses (SSE parser, transport response policy, JSON, the two provider stream
decoders, conversation persistence). `tests/fuzz/replay_main.cpp` replays each
corpus once as an ordinary `*-fuzz-replay` test, so the `asan` preset runs them
under ASan and UBSan. Nothing runs a coverage-guided search.

- **Test behavior at seams, not implementation inside them.** Tests target the
  machine, adapter, and transport interfaces.
- **Fakes over mocks.** A hand-written fake transport with scriptable responses
  survives refactors and reads as documentation.
- **Determinism.** No real sleeps, wall-clock time, or network in unit tests;
  time is an injected event. Transport and integration tests use local
  loopback HTTP/TLS servers.
- **Every bug becomes a test before it becomes a fix.**

## End-to-end testing

Start an OpenAI-compatible server, load a model, then:

```sh
SCRY_LOCAL_MODEL_BASE_URL=http://127.0.0.1:11434/v1 \
SCRY_LOCAL_MODEL_MODEL=qwen3:8b \
./scripts/test-e2e.sh
```

This builds and runs `tests/e2e/`, a chat and a required tool round through the
public API. Set `SCRY_LOCAL_MODEL_API_KEY` if the server requires one. It is not
part of ctest or CI.

## Testing downstream with `scry::testing`

`scry::testing` is an optional package component that hands a consumer the same
scripted-transport seam Scry's own suites use. It replaces the HTTP transfer and
nothing else.

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

`examples/testing_scripted.cpp` is a complete framework-free test in this shape;
`tests/testing/scripted_transport_tests.cpp` is the Catch2 equivalent.

## Rules

- Top-level builds use `-Wall -Wextra -Wconversion -Wshadow -Werror`.
- The kernel (`src/kernel/`) is C++23 without reflection, so clang-tidy can
  analyze it; the rest of `src/` is C++26 and GCC-only. The kernel compiles with
  `-Werror=c++26-extensions`, and `src/kernel/kernel.hpp` rejects a build with
  reflection enabled. `lint.sh` checks it includes only
  `<scry/{config,error,json,turn_id,unique_function}.hpp>` and kernel headers.
- clang-tidy uses the checked-in `.clang-tidy`, including cognitive complexity 25.
- `// TODO` must link an issue or a URL.

## Pull requests

Trunk-based, squash-merged, conventional-commit messages. A change lands with
tests, a bug fix with its regression test, a public API change with a compiling
example, and a behavior change with a `docs/architecture.md` update.

## Releases

1. Bump `project(VERSION ...)` in `CMakeLists.txt`; `<scry/version.hpp>` is
   generated from it.
2. Update the `find_package` version and FetchContent `GIT_TAG` in `README.md`,
   and the version assertions in `tests/public_api_contract.cpp`.
3. Write `docs/releases/vX.Y.Z.md`.
4. Tag and publish: `git tag vX.Y.Z && git push origin vX.Y.Z && gh release create vX.Y.Z --notes-file docs/releases/vX.Y.Z.md`.
