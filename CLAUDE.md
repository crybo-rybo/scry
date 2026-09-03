# CLAUDE.md

A static library (`scry::scry`) running the full LLM agentic tool loop — HTTP, SSE
streaming, tool dispatch, retries, transactional history — behind a poll-friendly
API for apps that own their main loop. The public API is C++26 and requires **GCC
16 or newer**, because P2996 reflection is how tools are declared, so there is no
Clang consumer build. Pre-1.0: no API or ABI stability promised.

## Sources of truth

Read the current branch, not remembered state. Do not implement or promise
behavior these do not cover: `docs/architecture.md` (what it is, how it works,
what it guarantees), `docs/contributing.md` (toolchain, presets, gates, and
what a change needs before it lands), and the public headers under
`include/scry/`.

## Commands

```sh
cmake --preset dev && cmake --build build/dev   # presets: dev ci asan tsan fuzz
ctest --test-dir build/dev --output-on-failure
ctest --test-dir build/dev -R 'runtime\.'       # one suite, or one case by name
cmake --build build/dev --target format         # format-check to verify only
./scripts/preflight.sh                          # the full local ring before a PR
```

Every preset pins `g++-16`; override with `-DCMAKE_CXX_COMPILER=...` when the
local GCC 16 is spelled differently.

## Directory map

- `include/scry/` — public headers, each compiling standalone with no third-party
  types; reflection lives here, in `detail/reflection_*.hpp`.
- `src/` by layer — `core/` (neutral model, seams, JSON codec), `machine/`
  (sans-I/O turn machine), `protocol/` (SSE), `provider/` (Anthropic,
  OpenAI-compatible), `runtime/` (worker, pump, registry, conversation),
  `transport/` (curl), `reflection/` (JSON bridge).
- `tests/`, `examples/`, `extras/showcase/` (a standalone project the root build
  never configures), `scripts/` (one per CI leg), `cmake/`, `docs/`.

## Guardrails

- Keep `src/**` free of reflection syntax so the `SCRY_CLANG_TOOLING` build
  (clang-tidy, libFuzzer) keeps compiling.
- Warnings are errors. lizard fails at cyclomatic 15 and 6 arguments; clang-tidy
  at cognitive complexity 25. `// TODO` must link an issue.
- Scry-originated failures are values (`std::expected` / `Result<T>`), never
  exceptions across the public or thread boundary.
- Bug fixes land with a regression test first, public API changes with a
  compiling example, behavior changes with a `docs/architecture.md` update.
- `project()` in `CMakeLists.txt` is the only place the release number lives;
  `<scry/version.hpp>` is generated from it and is not tracked.
- Never edit or commit anything under `build/`.

## PR conventions

Trunk-based, squash-merged, conventional-commit messages. The template asks for
what and why plus three checkboxes: preflight ran with skipped legs named, tests
added or updated, and the load-bearing docs updated when behavior changed.
