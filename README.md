# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. Scry uses **C++26 reflection (P2996)** to turn plain C++ structs into LLM-callable tools, and hides the full agentic loop — HTTP, streaming, tool dispatch, retries — behind a small, poll-friendly, never-blocking API. The name is the design: reflection (the mirror) + consulting an oracle (the LLM).

Built for the apps that live in C++ — games, GUI tools, simulators — where you can't block a frame, can't shell out to Python, and want tool use, not just chat.

**Status:** M0 (Skeleton) underway. The build/CI scaffold is in place; the documents below remain the design source of truth. Next up: the P2996 schema-generation spike, the libcurl SSE probe, and the compile-only public header sketch.

## Building

Requires CMake ≥ 3.25, Ninja, and a C++23 compiler (see the CI matrix for the blessed set).

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Presets: `dev`, `release`, `asan-ubsan`, `tsan`, `reflection` (the C++26/P2996 leg — experimental toolchains only). With [just](https://github.com/casey/just) installed, `just ci-fast` runs the whole per-commit CI ring locally — format check, build, tests, sanitizers, clang-tidy (requires `clang-format`/`clang-tidy` on PATH).

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the ~5 public concepts, interaction and threading model (with diagrams), reflection-based tool registration, provider abstraction, open questions, roadmap (M0–M5). **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure + consteval codegen, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, coverage and CRAP/complexity gating, static and dynamic analysis (sanitizers, fuzzing, mutation testing), CI shape, workflow, and the ratchet philosophy — metrics never regress. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row with milestone and verification method. When prose elsewhere conflicts with the register, the register wins. |
| [docs/adr/](docs/adr/) | Architecture Decision Records — the one-off forks in the road (build system, dependencies, test framework, toolchain strategy). The evolution register in ARCHITECTURE.md §11 indexes the *standing* simplifications. |

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.
