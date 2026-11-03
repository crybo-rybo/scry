# Handoff: `refactor/scrydantic-split`

Temporary branch notes for picking this work up on another machine. **Delete this
file before merging** (move whatever is still useful into the PR description).

The design record is [`scrydantic/NOTES.md`](scrydantic/NOTES.md): layout,
every coupling cut, deviations from the plan, the public API breaks, and the test
mapping. The user-facing breaking changes are in
[`docs/releases/unreleased.md`](docs/releases/unreleased.md). This file covers
only what is needed to verify the branch and get it to a PR.

## State of the branch

Branched from `main` @ `2e2553d`. Each phase commit was built and tested before
it was pushed:

| Commit | Phase |
|---|---|
| `05a4845` | Scrydantic project skeleton and lint boundary |
| `66af009` | JSON core moved into Scrydantic |
| `7ef96f8` | Reflected codec and schema moved into Scrydantic |
| `69d781e` | Tool discovery and binding moved into Scrydantic |
| `fda4c29` | Standalone tests, example, and install check |
| `6d54164` | Docs and release notes |

Verified in the cloud container (Ubuntu 24.04, GCC 16.0.1 from the
`ubuntu-toolchain-r/test` PPA, CMake 4.4, Ninja):

| Check | Result |
|---|---|
| `./scripts/test.sh` (dev) | 521/521 pass (baseline 507; no baseline test lost, see the test mapping in NOTES.md) |
| `PRESET=asan ./scripts/test.sh` | 521/521 pass |
| `PRESET=tsan ./scripts/test.sh` | 521/521 pass (run after the last commit; not recorded in NOTES.md) |
| `./scripts/format.sh --check`, `./scripts/lint.sh` | Pass with clang-format and clang-tidy **22.1.8 from PyPI**, not the apt.llvm.org packages CI uses |
| `cmake -S scrydantic` on its own | Builds, 60/60 tests pass, no binary links libcurl |
| Install, then a consumer with only `find_package(scrydantic)` | Builds and runs (steps below) |
| Install, then a consumer with only `find_package(scry)` | Builds and registers a reflected namespace (done by the worker; not committed) |
| `scry_e2e_smoke` | **Compiles only.** It needs a live model, so it has not been run |
| `extras/showcase` | Configures and builds (not in CI); the demos were not run |

Nothing was run on macOS or against a live model.

## Verifying on your machine

Toolchain: follow the "Toolchain" section of `docs/contributing.md`, which covers
macOS and Ubuntu. Two snags from the cloud setup:

- If `add-apt-repository` fails (`No module named 'apt_pkg'`), add the PPA by hand:
  fetch key `60C317803A41BA51845E371A1E9377A2BA9EF27F` from keyserver.ubuntu.com,
  dearmor it into `/usr/share/keyrings/`, and add a `signed-by=` source line for
  `https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu noble main`.
- Ubuntu 24.04's CMake is 3.28; the project needs 3.30 or newer
  (`pip install 'cmake>=3.30'` works).

Then, from the repository root:

```sh
git fetch origin && git switch refactor/scrydantic-split

./scripts/test.sh                      # expect 521/521
PRESET=asan ./scripts/test.sh          # expect 521/521
PRESET=tsan ./scripts/test.sh          # expect 521/521 (no TSan runtime on Apple Silicon GCC)
./scripts/format.sh --check            # with clang-format 22
./scripts/lint.sh                      # with clang-tidy 22

# Scrydantic alone
cmake -S scrydantic -B build/scrydantic -G Ninja -DCMAKE_CXX_COMPILER=g++-16
cmake --build build/scrydantic && ctest --test-dir build/scrydantic   # expect 60/60
build/scrydantic/examples/scrydantic_standalone_tools_example         # MCP-style tools/list + tools/call, exit 0

# Live smoke (needs a model server, for example `ollama serve` + `ollama pull qwen3:8b`)
SCRY_E2E_BASE_URL=http://127.0.0.1:11434/v1 SCRY_E2E_MODEL=qwen3:8b ./scripts/test-e2e.sh
```

Install-consumer check (the same steps passed in the cloud container):

```sh
cmake --install build/dev --prefix "$PWD/build/stage"   # installs both scry and scrydantic packages
mkdir -p build/consumer-src && cd build/consumer-src
cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.30)
project(scrydantic_consumer LANGUAGES CXX)
find_package(scrydantic 0.1 REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE scrydantic::scrydantic)
EOF
cat > main.cpp <<'EOF'
#include <iostream>
#include <scrydantic/scrydantic.hpp>

struct Point {
  [[= scrydantic::description{"Horizontal position"}]] double x{};
  double y{};
};

int main() {
  std::cout << scrydantic::input_schema_v<Point> << '\n';
  auto decoded = scrydantic::decode<Point>(scrydantic::Json{.text = R"({"x":1,"y":2})"});
  return decoded && decoded->y == 2.0 ? 0 : 1;
}
EOF
cmake -S . -B ../consumer -G Ninja -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_PREFIX_PATH="$PWD/../stage"
cmake --build ../consumer && ../consumer/consumer   # prints the schema, exit 0
```

## Before opening the PR

- [ ] All checks above pass on your machine, including the **live e2e smoke**,
      which has never run on this branch. Running `examples/quickstart.cpp`
      against a model is a good second smoke: the examples are compiled in CI
      but not run.
- [ ] format and lint pass with the **CI versions** (`clang-format-22`,
      `clang-tidy-22` from apt.llvm.org). The cloud run used PyPI 22.1.8, so
      expect the lint job to be the most likely CI surprise.
- [ ] If you develop on macOS: `./scripts/lint.sh` now also runs clang-tidy over
      `scrydantic/src/json/` against libc++. That has not been tried.
- [ ] Review commit by commit rather than as one diff (339 files,
      +4633/−2076; much of it moved tests, fixtures, and the renames). Read
      NOTES.md "Deviations from the plan's default design" and "Public API breaks".
- [ ] Settle the "decide before the PR" questions below.
- [ ] `.github/workflows/ci.yml` needed no change: the root build covers
      Scrydantic, and `scripts/format.sh` and `scripts/lint.sh` were extended.
      CI runs only on `pull_request` (and pushes to `main`), so the first CI run
      happens when the PR opens.
- [ ] Fill in `.github/pull_request_template.md`: the three scripts, tests
      added or updated, and `docs/architecture.md` (already updated).
- [ ] Delete this file.

## Open questions

### Decide before the PR

1. **Two error types for tool authors.** Reflected tools now return
   `scrydantic::Result`/`Status` and refuse with `scrydantic::tool_error()`.
   Dynamic tools (`add_dynamic`) and answer validators still use
   `scry::Result`/`Status` and `scry::tool_error()`. A user who writes both
   kinds sees two error types. The alternative is to accept both in the
   handler rules, or move dynamic handlers to `scrydantic::Error` too. Is the
   split acceptable for now?
2. **Scrydantic has no LICENSE in its install** (NOTES.md, "Missing project
   files"). Scry installs the root LICENSE into its own doc directory only.
   This is cheap to fix before merge; a separate repository needs it anyway.
3. **Versioning.** Scrydantic starts at 0.1.0, and Scry's package requires that
   exact version. Scry itself stays at 0.7.0 on this branch. The breaking
   change implies 0.8.0 when `docs/releases/unreleased.md` is released. Is the
   version bump part of this PR or the release?
4. **Frozen fuzz seeds.** 18 provider, conversation, and SSE seeds were copied
   into `scrydantic/tests/fuzz/corpus/` for the JSON prefix test. New Scry seeds
   no longer feed it. Is that acceptable, or should Scry's corpus stay the
   source?
5. **Null toolbox.** `scrydantic::bind(std::shared_ptr<Toolbox>{})` binds
   nothing rather than failing (Scry still rejects it with `invalid_argument`).
   Is that fine for standalone users?
6. **Double type erasure.** Each reflected call now goes through
   `UniqueFunction` wrapped around `std::move_only_function`: one extra
   indirection. It is probably negligible next to JSON decoding, but nobody
   measured it.

### Decide before moving Scrydantic to its own repository

Full detail is in NOTES.md, "Open questions for a separate repository":

- Scry calls `scrydantic::detail` APIs: the diagnostic builders,
  `codec_failure`, `read_value`/`write_value`, `fields_v`/`alternatives_v`/`tag_of`,
  and `json_text.hpp`. Either promote a small integration API or pin exact
  versions.
- A compatibility policy between Scry and Scrydantic minors, and FetchContent
  instructions with a Scrydantic tag.
- Check the `scrydantic` name for collisions.
- Scrydantic's own CI, `CMakePresets.json`, `.clang-tidy`, README, and LICENSE.
  Its `.clang-format` already exists.
- `std::move_only_function` (absent from macOS libc++) versus moving
  `UniqueFunction` down into Scrydantic.
- Duplicated test assets (`replay_main.cpp`, `ExpectCompileFailure.cmake`,
  seeds) and the shared Catch2 pin.

### API design (the first-principles pass, after the split settles)

These are not blockers. The extraction preserved today's behavior on purpose:

- Schema dialects: OpenAI strict mode, Gemini's subset, MCP conventions
  (`schema_v<T, Dialect>` is the natural extension point).
- Constraint and validator annotations (bounds, patterns, lengths, custom
  checks). Today ranges come only from the C++ type.
- An error kind on `scrydantic::Error` ("not JSON", "schema mismatch",
  "encode failure", "handler refusal").
- `$defs`/`$ref` for shared nested types instead of inlining.
- Whether to own the JSON parser or decode against a reader interface.
- Which of the Scry-internal codec features are public: borrowed
  `string_view` encoding, raw `Json` splicing, canonical output.
