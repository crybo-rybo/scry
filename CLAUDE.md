# Scry — Claude Code guide

C++ LLM harness for apps with their own main loops. C++23 core; severable C++26 (P2996) reflection layer behind `SCRY_ENABLE_REFLECTION`. Four load-bearing docs: DESIGN.md (what), ARCHITECTURE.md (how it's shaped), ENGINEERING.md (how we work), REQUIREMENTS.md (normative register).

## Non-negotiables

- **REQUIREMENTS.md is normative.** When prose conflicts with the register, the register wins. Requirement IDs (`SCRY-<AREA>-NNN`) are permanent; withdrawn rows are struck, never reused. Load the `register-conventions` skill before editing it.
- **Definition of Done** (ENGINEERING.md §7): gates green (`just ci-fast`); docs updated when behavior or a decision changes — a stale doc is a bug; requirement verification cells name their gate once it exists; deliberate simplifications add an evolution-register row (ARCHITECTURE.md §11); decisions get an ADR in `docs/adr/` (use `/adr`).
- **Dependencies are capped** at libcurl + Glaze runtime, test frameworks on top (PORT-003). Any addition needs a written justification — ADR 0002 is where they accrue.
- **Public headers**: no third-party types or headers, ever (API-002); no virtual/protected (API-001); exception-free boundary via `std::expected` (API-003).
- **Warnings are errors**: `-Wall -Wextra -Wconversion -Wshadow` (QA-006). Tests are deterministic — no sleeps, wall clock, or network in unit tests (QA-008).
- **Threading contract** (once code lands): all user callbacks fire inside `update()` on the calling thread; exactly three sanctioned thread-crossing objects (ARCHITECTURE.md §2–3). Threaded tests always run under TSan.

## Working here

- Build/test: `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`. Presets: `dev`, `release`, `asan-ubsan`, `tsan`, `reflection` (experimental toolchains).
- `just ci-fast` mirrors the per-commit CI ring — run it before pushing.
- All FetchContent deps build with the project-wide `CMAKE_CXX_STANDARD` (root CMakeLists pins it; standard mismatches cause link errors like the Catch2 `StringMaker<string_view>` case).
- Workflow: trunk-based, PRs even solo, squash-merge, conventional-commit messages (`build:`, `docs:`, `feat:`, `fix:`, `chore:`...).
- The `register-auditor` agent checks a branch for QA-012 traceability before a PR — run it when a changeset is ready.
