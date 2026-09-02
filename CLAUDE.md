# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` at the repo root is the primary agent guidance file and is kept in sync with this one. Read it first; this file adds the commands and architecture map it omits.

## What Scry is

A C++23 static library (`scry::scry`) that runs the full LLM agentic tool loop (HTTP, SSE streaming, tool dispatch, retries, transactional history) behind a poll-friendly async API for apps that own their main loop (games, GUI tools). Two provider dialects: Anthropic Messages and a strict OpenAI-compatible Chat Completions subset (also drives Ollama/vLLM/llama.cpp). An optional, experimental C++26 P2996 reflection component (`scry::reflection`, GCC 16+ only) derives tool schemas from plain structs. Pre-1.0, no API/ABI stability promised.

## Sources of truth

Do not implement or promise behavior the docs do not cover.

1. `docs/requirements.md` is normative. Its RFC-2119 `SCRY-<AREA>-NNN` rows win over any prose.
2. `docs/design/` defines product behavior and the five public concepts (`Config`, `Conversation`, `ToolRegistry`, `Turn`, `Harness`).
3. `docs/architecture/` defines boundaries, dependency direction, and the evolution register of deliberate simplifications.
4. `docs/development/` defines quality gates and the definition of done.

Behavior, architecture, requirement, or dependency changes must update the relevant document in the same change. Reference affected `SCRY-<AREA>-NNN` rows in PR summaries. `docs/releases/` holds release notes.

## Commands

Build dirs live under `build/<preset>` (Ninja generator, `compile_commands.json` exported; `.clangd` points at `build/dev`).

Edit/build/test loop:

```sh
cmake --preset dev                                   # or: just configure
cmake --build build/dev                              # or: just build
ctest --test-dir build/dev --output-on-failure       # or: just test
```

Run a single test or suite. Catch2 tests are registered with ctest under a per-suite prefix (`runtime.`, `machine.`, `protocol.`, `provider.`, `transport.`, `integration.`, `reflection.`, `showcase.`); `public-api-contract` is a plain executable test.

```sh
ctest --test-dir build/dev -R 'runtime\.'                          # one suite
ctest --test-dir build/dev -R 'event queue coalesces'              # one Catch2 case by name substring
./build/dev/tests/scry_runtime_tests "event queue coalesces adjacent deltas"   # run the Catch2 binary directly
./build/dev/tests/machine/scry_turn_machine_tests --list-tests
```

Formatting (clang-format, LLVM base, 88 cols; targets exist only when `SCRY_ENABLE_FORMAT_CHECK=ON`, which the presets set):

```sh
cmake --build build/dev --target format         # just format
cmake --build build/dev --target format-check   # just format-check
```

Gates:

```sh
./scripts/ci-local.sh     # just ci-fast: diff check, lizard complexity, unlinked-TODO check, format-check,
                          # ci preset build+ctest, install to build/stage, reflection-leak audit,
                          # downstream find_package(scry) consumer build. The quick inner loop.
./scripts/preflight.sh    # just ci: the full ring, and what to run before every PR. Adds the Doxygen site,
                          # the GCC 14 core leg, profiling smoke, clang-tidy 18, ASan/UBSan, TSan (x3 repeat),
                          # the fuzz corpus replay, and the GCC 16 reflection leg. Legs whose toolchain the host
                          # lacks are skipped, not failed, and named again in the closing summary; hosted CI is
                          # authoritative for those. On macOS, `brew install gcc@14 llvm@18` turns the GCC 14 and
                          # clang-tidy legs on locally (CI pins clang-tidy 18, so llvm@18 is probed before llvm).
just tidy | just asan | just tsan | just reflection | just docs | just showcase   # individual legs
```

Other presets: `dev-logging` (compiles `SCRY_ENABLE_LOGGING`, writes only to `$SCRY_LOG_FILE`), `profile` (Google Benchmark, see `docs/development/performance-profiling.md` and `just profile*`), `fuzz`, `showcase` (Dear ImGui + NPC examples), `nightly-local-model`.

## Architecture map

Read `docs/architecture/overview.md` for the rationale; this is the file-level orientation.

**Thread model: one worker actor, one pump.** `src/runtime/worker.cpp` runs a `std::jthread` that exclusively owns all networking and loop state. The app thread calls `Harness::send()` (pushes a command) and `Harness::update()` (drains the event queue and runs every callback and tool handler on the calling thread). The only cross-thread state is the command queue, the event queue (`src/runtime/queue.cpp`, which coalesces text deltas and enforces byte ceilings), and one `atomic<bool>` cancel flag per turn. Anything else found crossing threads is a bug by definition; TSan enforces this.

**Pump side** (`src/runtime/pump.cpp`, `pump_state.cpp`, `tool_dispatch.cpp`): routes events by `TurnId` to per-turn routes holding the callbacks moved in at `send()`, runs tool handlers in provider order against the machine's budget, and commits or rolls back `Conversation` history transactionally at the terminal event. `Turn` is a move-only weak handle exposing only `id()` and `cancel()`.

**Sans-I/O turn machine** (`src/machine/turn_machine.cpp`): a pure state machine (queued, awaiting-model, streaming, retry-wait, awaiting-tool, terminal) that consumes events and emits commands and never does I/O or sleeps; retry backoff is driven by injected time events. The worker is a thin driver around it. Test the loop logic here with event sequences, not through the network.

**Provider adapters** (`src/provider/`): the Strategy seam declared in `src/core/provider.hpp`. Each dialect has `*_request.cpp` (neutral request to wire JSON), `*_stream.cpp` (wire SSE/JSON to neutral events), and `*_content.cpp`. Adapters are stateless; per-turn parse state lives in `ProviderDecodeState`. Selection is a factory keyed on the `Config` dialect enum. Golden fixtures live in `tests/fixtures/`.

**Transport** (`src/transport/`): the second virtual seam, `src/core/transport.hpp`, exists so tests can inject `tests/support/transport/fake_transport.hpp` or the loopback server. libcurl is RAII-wrapped and its types never leave the `.cpp`. `src/protocol/sse.cpp` is a pure incremental SSE parser, fuzzed under `tests/protocol/`.

**Neutral model** (`src/core/model.hpp`): `Message`/`ContentBlock`/`ToolSchema`. Adapters see only this; nothing above them sees JSON or HTTP. JSON is Glaze, private-only, wrapped by `src/core/json_codec.cpp` and the public `scry::Json` boundary type.

**Tool registry** (`src/runtime/tool_registry.cpp`): type-erased records (name, description, canonical schema string, `UniqueFunction` handler). Additive-only; `send()` snapshots immutable records per turn. The reflection layer (`include/scry/reflection.hpp`, `include/scry/detail/reflection_*.hpp`, `src/reflection/json_bridge.cpp`) is a consteval code generator that lowers onto this same registry via `scry::reflection::add`.

**Public header boundary.** `include/scry/*.hpp` (listed in `SCRY_CORE_PUBLIC_HEADERS` in `CMakeLists.txt`) must each compile standalone and contain no third-party types; `cmake/CheckPublicHeaders.cmake` and per-header object targets enforce this, and `ci-local.sh` fails if any reflection artifact leaks into a core-only install. Callable boundaries use `scry::UniqueFunction`, not `std::move_only_function` (macOS libc++ gap).

**Test seams.** `src/runtime/test_access.hpp` (`HarnessTestAccess::create`) builds a `Harness` with an injected provider and transport. Shared fixtures are in `tests/support/` and `tests/runtime/runtime_test_support.hpp`. Unit tests use deterministic fakes only: no real sleeps, wall-clock time, or network. Integration tests use the in-process loopback server.

## Engineering guardrails

- Keep the reflection-OFF C++23 build green; reflection-only code stays behind `scry::reflection` and `SCRY_ENABLE_REFLECTION`.
- Warnings are errors (`-Wall -Wextra -Wconversion -Wshadow`) on GCC 14, Clang 18/libc++, and AppleClang. A warning on one compiler still fails.
- Complexity limits are gated: lizard cyclomatic fail at 15, 6 args; clang-tidy cognitive complexity fail at 25. `// TODO` comments must link an issue or URL.
- Scry-originated failures are values (`std::expected` / `Result<T>`), never exceptions across the public boundary or thread boundary. Tool-handler exceptions become tool-error results; user callback exceptions propagate synchronously out of `update()`.
- Bug fixes add a regression test first; public API changes add a compiling example under `examples/`; new dependencies are pinned by commit hash and justified in the same change.
- Deliberate shortcuts go in the evolution register (`docs/architecture/quality-and-evolution.md`).
- `include/scry/version.hpp` and the `project()` version in `CMakeLists.txt` must be bumped together; configure fails otherwise.
- Never edit or commit anything under `build/`.

## PR conventions

Trunk-based, squash-merged, conventional-commit messages. The PR template (`.github/pull_request_template.md`) requires linking affected requirements and checking off the definition of done: `just ci` passes or unavailable legs are named, tests added, load-bearing docs and evolution register updated, examples for API changes, dependency justifications.
