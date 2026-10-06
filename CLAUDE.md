# CLAUDE.md

A static library (`scry::scry`) running the full LLM agentic tool loop — HTTP, SSE
streaming, tool dispatch, retries, transactional history — behind a poll-friendly
API for apps that own their main loop. The public API is C++26 and requires **GCC
16 or newer**, because P2996 reflection is how tools are declared, so there is no
Clang consumer build. Pre-1.0: no API or ABI stability promised.

## Sources of truth

Read the current branch, not remembered state. Do not implement or promise
behavior these do not cover: `docs/architecture.md` (what it is, how it works,
what it guarantees), `docs/contributing.md` (toolchain, commands, and
what a change needs before it lands), and the public headers under
`include/scry/`.

## Commands

```sh
./scripts/test.sh                   # configure, build, ctest (dev preset)
./scripts/test.sh -R 'runtime\.'    # one suite, or one case by name
PRESET=asan ./scripts/test.sh       # presets: dev asan tsan
./scripts/format.sh --fix           # --check to verify; CI pins clang-format 18
./scripts/lint.sh                   # clang-tidy over src/kernel/ + repo rules
./scripts/test-e2e.sh               # live model; requires URL and model
```

CI runs format, lint, and `test.sh` under each preset. The presets select
`g++-16`; set `CXX` or pass `-DCMAKE_CXX_COMPILER=...` when needed.

## Directory map

- `include/scry/` — public headers, each compiling standalone with no third-party
  types; reflection lives here, in `detail/reflection_*.hpp`.
- `src/` by layer — `kernel/` (C++23: error factory, JSON codec, SSE, retry,
  transport seam and curl), `core/` (neutral model, provider seam), `machine/`
  (sans-I/O turn machine), `provider/` (Anthropic, OpenAI-compatible),
  `runtime/` (worker, pump, registry, conversation), `reflection/` (JSON bridge).
- `tests/`, `examples/`, `extras/showcase/` (a standalone project the root build
  never configures), `scripts/` (build, test, format, lint), `cmake/`, `docs/`.

## Guardrails

- `src/kernel/` is C++23 without reflection in every build, enforced by the
  compiler; it is all clang-tidy analyzes. Kernel code may include only
  `<scry/error.hpp>`, `<scry/json.hpp>`, `<scry/config.hpp>`,
  `<scry/turn_id.hpp>`, `<scry/unique_function.hpp>`, and other kernel headers,
  never the rest of `src/` (`scripts/lint.sh` checks this). Everything else in
  `src/` is C++26 and is expected to use reflection where it replaces
  hand-written shape code.
- Top-level builds treat warnings as errors; clang-tidy allows cognitive
  complexity 25. `// TODO` must link an issue.
- Scry-originated semantic failures are values (`std::expected` / `Result<T>`).
  Allocation failure is outside that contract. Observer exceptions propagate from
  `update()`; tool-handler exceptions become tool-error results.
- Bug fixes land with a regression test first, public API changes with a
  compiling example, behavior changes with a `docs/architecture.md` update.
- `project(VERSION ...)` in `CMakeLists.txt` is the version source of truth;
  `<scry/version.hpp>` is generated from it and is not tracked.
- Never edit or commit anything under `build/`.

## PR conventions

Trunk-based, squash-merged, conventional-commit messages. The template asks for
what and why plus three checkboxes: test, format, and lint pass; tests added or
updated; `docs/architecture.md` updated when behavior changed.
