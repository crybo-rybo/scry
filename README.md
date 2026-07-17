# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. Scry uses **C++26 reflection (P2996)** to turn plain C++ structs into LLM-callable tools, and hides the full agentic loop — HTTP, streaming, tool dispatch, retries — behind a small, poll-friendly async API with an explicitly named synchronous convenience. The name is the design: reflection (the mirror) + consulting an oracle (the LLM).

Built for the apps that live in C++ — games, GUI tools, simulators — where you can't block a frame, can't shell out to Python, and want tool use, not just chat.

**Status:** M0 foundation in progress. The public contracts, canonical example,
target-based CMake package, and local/hosted validation skeleton are present,
along with bounded reflection and libcurl/SSE feasibility probes. The runtime
is intentionally not implemented yet: the PImpl handle methods are declarations,
and the example is compile-checked rather than linked.

## Build the foundation

Run the same core workflow used by CI:

```sh
./scripts/ci-local.sh
```

That command checks formatting and public-header boundaries, builds the
compile-only API example, runs contract tests, installs the package to a staging
prefix, and builds a downstream `find_package(scry)` consumer. If
[`just`](https://github.com/casey/just) is installed, `just ci-fast` is the
equivalent convenience command.

The reflection-OFF surface targets stable C++23 compilers. The separate
reflection feasibility target requires GCC 16+ with `-freflection`; it is not
part of the default local build. `just curl` runs the independent system-libcurl
and write-callback feasibility check.

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the five core public concepts, interaction and threading model (with diagrams), reflection-based tool registration, provider abstraction, open questions, roadmap (M0–M5). **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure + consteval codegen, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, coverage and CRAP/complexity gating, static and dynamic analysis (sanitizers, fuzzing, mutation testing), CI shape, workflow, and the ratchet philosophy — metrics never regress. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row with milestone and verification method. When prose elsewhere conflicts with the register, the register wins. |
| [ADR 0001](docs/adr/0001-public-object-graph-and-lifetimes.md) | Accepted public ownership, registry snapshot, Turn detach, and callback-lifetime decisions. |
| [ADR 0002](docs/adr/0002-build-and-dependency-foundation.md) | Build, package, dependency-acquisition, and initial test-harness decisions. |

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.
