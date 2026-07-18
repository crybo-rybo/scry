# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. The stable C++23
surface supports explicit-schema tools and hides the full agentic loop — HTTP,
streaming, tool dispatch, retries — behind a small, poll-friendly async API
with an explicitly named synchronous convenience. C++26 reflection (P2996) is
an isolated M3 component that lowers typed tools onto that same runtime
registry. The name is the design: reflection (the mirror) +
consulting an oracle (the LLM).

Built for the apps that live in C++ — games, GUI tools, simulators — where you can't block a frame, can't shell out to Python, and want tool use, not just chat.

**Status:** M3 reflection complete. The C++23 runtime snapshots explicit-schema
registrations per accepted turn, serializes them to Anthropic Messages,
executes ordered tool batches inside the app's `update()` call, and
automatically resends ordered results until a final answer. Tool rounds remain
transactional, bounded, cancellable, and persistable through the versioned
`Conversation::to_json()` / `Conversation::from_json()` boundary. Typed P2996
schema generation, strict marshalling, and the optional reflection package
component are implemented under
[ADR 0007](docs/adr/0007-m3-reflection-contract.md). The supported GCC 16 gate
builds the reflected example and standalone header, runs 27 schema, codec,
bridge, registration, and compile-fail tests, audits a clean component install,
runs a downstream
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)` consumer, and
repeats the reflection suite under ASan+UBSan. Its pinned coverage leg gates
the codec's adjusted source decisions at 95% and functions at 100%, plus the
compiled bridge's GCC/gcovr CFG branches at 95%. The current gated results are
32/32 decisions, 62/62 functions, and 97/97 bridge branches. One
inline-justified GCC-generated switch artifact is excluded by a validator that
requires exactly that exclusion; unadjusted codec decisions and combined CFG
arcs remain visible diagnostics. The reflection-OFF install and consumer
remain clean C++23 surfaces with no reflection artifacts.

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
sanitizers, short protocol fuzzing, and host-specific curl/reflection legs. It
runs all available legs and reports host-specific toolchains that are
unavailable locally; hosted CI is authoritative for those environments.
The M3 reflection gate is live through that same preflight entry point.
`just ci` is the optional convenience wrapper.

The reflection-OFF surface targets stable C++23 compilers. The accepted M3
package shape keeps it that way: a reflection-enabled build uses GCC 16+ with
`-std=c++26 -freflection`, and consumers opt in with
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)` and
`scry::reflection`. Core-only builds and installations contain no reflection
component or experimental language flags. The shared reflection build, test,
coverage, ASan+UBSan, install-audit, and downstream-consumer gate is live.
clang-p2996 remains manual, non-gating compatibility work, produces no package
artifacts, and is not claimed as M3 verification.
`just curl` exercises the production Curl runtime integration as well as the
retained low-level feasibility probe.

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the five core public concepts, interaction and threading model (with diagrams), implemented explicit-schema and reflected-tool ergonomics, provider abstraction, open questions, and roadmap (M0–M5). **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure, optional consteval codegen and JSON bridge, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, coverage and CRAP/complexity gating, static and dynamic analysis (sanitizers, fuzzing, mutation testing), CI shape, workflow, and the ratchet philosophy — metrics never regress. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row with milestone and verification method. When prose elsewhere conflicts with the register, the register wins. |
| [ADR 0001](docs/adr/0001-public-object-graph-and-lifetimes.md) | Accepted public ownership, registry snapshot, Turn detach, and callback-lifetime decisions. |
| [ADR 0002](docs/adr/0002-build-and-dependency-foundation.md) | Build, package, dependency-acquisition, and initial test-harness decisions. |
| [ADR 0003](docs/adr/0003-test-framework-deferred.md) | Historical M0 decision deferring the test framework until M1. |
| [ADR 0004](docs/adr/0004-live-quality-ratchet.md) | Live merge-base quality comparison for diff branch coverage, CRAP, and ratcheted debt metrics. |
| [ADR 0005](docs/adr/0005-m1-runtime-and-test-foundation.md) | Compiled M1 runtime, pinned dependencies, internal contracts, and chat-only milestone boundary. |
| [ADR 0006](docs/adr/0006-m2-agentic-tool-loop.md) | M2 registry snapshots, agentic tool rounds, app-thread dispatch, retry accounting, transactional commit, and Conversation persistence. |
| [ADR 0007](docs/adr/0007-m3-reflection-contract.md) | Accepted M3 schema/type mapping, strict marshalling, description precedence, optional package component, and no-Glaze public boundary. |

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.
