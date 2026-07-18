# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. The stable C++23
surface supports explicit-schema tools and hides the full agentic loop — HTTP,
streaming, tool dispatch, retries — behind a small, poll-friendly async API
with an explicitly named synchronous convenience. C++26 reflection (P2996) is
an isolated M3 capability that will lower typed tools onto that same runtime
registry. The name is the design: reflection (the mirror) + consulting an
oracle (the LLM).

Built for the apps that live in C++ — games, GUI tools, simulators — where you can't block a frame, can't shell out to Python, and want tool use, not just chat.

**Status:** M2 tools complete. The C++23 runtime snapshots explicit-schema
registrations per accepted turn, serializes them to Anthropic Messages,
executes ordered tool batches inside the app's `update()` call, and
automatically resends ordered results until a final answer. Tool rounds remain
transactional, bounded, cancellable, and persistable through the versioned
`Conversation::to_json()` / `Conversation::from_json()` boundary. The
[canonical example](examples/main_loop.cpp) compiles and the installed package
is exercised by a downstream `find_package(scry)` consumer. Typed P2996 schema
generation and marshalling remain M3 work.

## Build and preflight

Run the fast, platform-stable core workflow:

```sh
./scripts/ci-local.sh
```

That command checks formatting and public-header boundaries, builds the linked
API example, runs the behavioral and contract suites, installs the package to a
staging prefix, and runs a downstream `find_package(scry)` consumer. If
[`just`](https://github.com/casey/just) is installed, `just ci-fast` is the
equivalent convenience command.

Before handing off a pull request, run the complete local preflight:

```sh
./scripts/preflight.sh
```

That one command adds the live branch-coverage/CRAP ratchet, clang-tidy,
sanitizers, short protocol fuzzing, and the curl and reflection feasibility
legs. It runs all legs and reports host-specific toolchains that are unavailable
locally; hosted CI is authoritative for those environments. `just ci` is the
optional convenience wrapper.

The reflection-OFF surface targets stable C++23 compilers. The separate
reflection feasibility target requires GCC 16+ with `-freflection`; it is not
part of the default local build. `just curl` exercises the production Curl
runtime integration as well as the retained low-level feasibility probe.

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the five core public concepts, interaction and threading model (with diagrams), implemented explicit-schema tools, future reflection ergonomics, provider abstraction, open questions, and roadmap (M0–M5). **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure, future consteval codegen, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, coverage and CRAP/complexity gating, static and dynamic analysis (sanitizers, fuzzing, mutation testing), CI shape, workflow, and the ratchet philosophy — metrics never regress. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row with milestone and verification method. When prose elsewhere conflicts with the register, the register wins. |
| [ADR 0001](docs/adr/0001-public-object-graph-and-lifetimes.md) | Accepted public ownership, registry snapshot, Turn detach, and callback-lifetime decisions. |
| [ADR 0002](docs/adr/0002-build-and-dependency-foundation.md) | Build, package, dependency-acquisition, and initial test-harness decisions. |
| [ADR 0003](docs/adr/0003-test-framework-deferred.md) | Historical M0 decision deferring the test framework until M1. |
| [ADR 0004](docs/adr/0004-live-quality-ratchet.md) | Live merge-base quality comparison for diff branch coverage, CRAP, and ratcheted debt metrics. |
| [ADR 0005](docs/adr/0005-m1-runtime-and-test-foundation.md) | Compiled M1 runtime, pinned dependencies, internal contracts, and chat-only milestone boundary. |
| [ADR 0006](docs/adr/0006-m2-agentic-tool-loop.md) | M2 registry snapshots, agentic tool rounds, app-thread dispatch, retry accounting, transactional commit, and Conversation persistence. |

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.
