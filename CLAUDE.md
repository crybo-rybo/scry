# CLAUDE.md

Scry is a static library (`scry::scry`). It runs the full agentic tool loop of an
LLM: HTTP, SSE streaming, tool dispatch, retries, and transactional history. It
puts this loop behind an API that an app can poll. This API is for apps that own
their main loop. The public API is C++26 and must have **GCC 16 or newer**. The
reason is that the API uses P2996 reflection to declare tools. Thus, there is no
Clang consumer build. Scry is pre-1.0, so it does not promise API or ABI
stability.

## Sources of truth

Read the current branch. Do not use the state that you remember. These are the
sources of truth:

- `docs/architecture.md`: what Scry is, how it works, and what it guarantees.
- `docs/contributing.md`: the toolchain, the presets, the gates, and what a
  change must have before it lands.
- The public headers under `include/scry/`.

Do not implement or promise behavior that these sources do not cover.

## Commands

```sh
cmake --preset dev && cmake --build build/dev   # presets: dev ci asan tsan fuzz
ctest --test-dir build/dev --output-on-failure
ctest --test-dir build/dev -R 'runtime\.'       # one suite, or one case by name
./scripts/format.sh --fix                       # --check to verify only
./scripts/test.sh                              # build and run unit/integration tests
./scripts/test-e2e.sh                           # live model; requires URL and model
./scripts/ci.sh                                 # all local CI checks before a PR
```

The `dev`, `ci`, `asan`, and `tsan` presets select `g++-16`. If necessary,
override this with `-DCMAKE_CXX_COMPILER=...`. For the `fuzz` preset, select a
Clang compiler explicitly. That compiler must have a libFuzzer runtime.

## Directory map

- `include/scry/`: the public headers. Each header compiles standalone and has
  no third-party types. Reflection is here, in `detail/reflection_*.hpp`.
- `src/`, by layer: `kernel/` (C++23: error factory, JSON codec, SSE, retry,
  transport seam and curl), `core/` (neutral model, provider seam), `machine/`
  (sans-I/O turn machine), `provider/` (Anthropic, OpenAI-compatible),
  `runtime/` (worker, pump, registry, conversation), `reflection/` (JSON bridge).
- `tests/`, `examples/`, `extras/showcase/` (a standalone project that the root
  build never configures), `scripts/` (local commands), `.github/scripts/` (CI
  helpers), `cmake/`, `docs/`.

## Guardrails

- `src/kernel/` is C++23 without reflection in every build. The compiler
  enforces this rule. The `SCRY_CLANG_TOOLING` build (clang-tidy, libFuzzer)
  compiles only the kernel. Kernel code can include only `<scry/error.hpp>`,
  `<scry/json.hpp>`, `<scry/config.hpp>`, `<scry/turn_id.hpp>`,
  `<scry/unique_function.hpp>`, and other kernel headers. It must not include
  the remaining files in `src/`. The `kernel.include-boundary` check finds such
  includes. All other code in `src/` is C++26. Use reflection there when it
  replaces hand-written shape code.
- Top-level builds treat warnings as errors. lizard allows a maximum cyclomatic
  complexity of 15 and a maximum of 6 arguments. clang-tidy allows a maximum
  cognitive complexity of 25. A `// TODO` must link an issue.
- Semantic failures that start in Scry are values (`std::expected` /
  `Result<T>`). Allocation failure is not part of that contract. Observer
  exceptions propagate from `update()`. Tool-handler exceptions become
  tool-error results.
- A bug fix lands with a regression test first. A public API change lands with
  an example that compiles. A behavior change lands with an update to
  `docs/architecture.md`.
- `project(VERSION ...)` in `CMakeLists.txt` is the version source of truth.
  CMake generates `<scry/version.hpp>` from it. Git does not track that file.
- Never edit or commit anything under `build/`.

## PR conventions

The project is trunk-based. It uses squash merges and conventional-commit
messages. The template asks what the change does and why. It also has three
checkboxes:

- `./scripts/ci.sh` ran, and the PR names each leg that it skipped.
- The PR adds or updates tests.
- If the behavior changed, the PR updates the load-bearing docs.
