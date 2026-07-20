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

**Status:** M4 breadth and the M5 showcase are complete. M5 is implemented under
[ADR 0010](docs/adr/0010-m5-showcase-contract.md) with opt-in C++23 examples for
a host-owned Dear ImGui chat panel and a deterministic grid NPC driven through
explicit tools. These examples consume only `scry::scry`: they add no public
API, installed artifact, or runtime dependency. The shared showcase gate passes
locally and in hosted CI.

The C++23 runtime selects Anthropic Messages or the strict OpenAI-compatible
Chat Completions subset from `Config`, including local servers with no API key.
Explicit and reflected tools accept `ToolRegistrationOptions`: app-thread
execution remains the default, while an explicit worker-thread opt-in preserves
provider order, app-thread observer delivery, accepted-turn snapshots,
transactional resend and commit, cancellation semantics, and exclusive handler
ownership.

M4's deterministic closure passes 277/277 development tests and 48/48 provider
tests. It includes exact OpenAI request/stream cases, a checked corpus
and short `scry_openai_fuzz` target, a fragmented transactional OpenAI tool
round, concurrent Anthropic/OpenAI isolation, a public Curl path/header/SSE
round, and worker-mode mixed/all-worker ordering, thread-ID, snapshot,
cancellation, detached-turn, budget, and cooperating-shutdown coverage. The
provider seam is streaming-only: the dead non-streaming decode path was
removed (ARCHITECTURE.md §11 records the reintroduction condition). The
absolute quality gates ([ADR 0011](docs/adr/0011-absolute-quality-gates.md))
pass from a single instrumented build: 93.322% diff branch coverage against
the 90% floor, the 95% component floors, the 88% aggregate branch-coverage
backstop, and a maximum CRAP of 13.125 against the limit of 30.

The scheduled/manual M4 nightly pipeline is also implemented: CodeQL, long
SSE/Anthropic/OpenAI fuzzing, an on-demand Mull mutation job, and a bounded
OpenAI-compatible smoke using checksum-pinned Ollama v0.32.1 pulling
`qwen3:1.7b-q4_K_M`. This records a live pipeline, not a claim that a hosted
nightly execution has already completed. Separately,
`scripts/ci-local-model.sh` passed locally through the public
OpenAI-compatible chat and required-tool paths using Ollama 0.22.1 and
`qwen3:1.7b-q4_K_M`; the checksum-pinned Ollama v0.32.1 hosted nightly
remains unexecuted and unclaimed.

The M3 reflection component remains complete. Typed P2996 schema generation,
strict marshalling, and the optional reflection package component are
implemented under
[ADR 0007](docs/adr/0007-m3-reflection-contract.md). The supported GCC 16 gate
builds the reflected example and standalone header, runs 27 schema, codec,
bridge, registration, and compile-fail tests, audits a clean component install,
runs a downstream
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)` consumer, and
repeats the reflection suite under ASan+UBSan. Its pinned coverage leg gates
the codec's source decisions at 85% and functions at 95% with stock gcovr
thresholds, plus the compiled bridge's GCC/gcovr CFG branches at 95%
([ADR 0011](docs/adr/0011-absolute-quality-gates.md)); the decision floor
allows for the one inline-justified GCC-generated switch artifact that
gcovr's decision analysis still counts. The reflection-OFF install and
consumer remain clean C++23 surfaces with no reflection artifacts.

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

That one command adds the absolute coverage/CRAP quality gate, clang-tidy,
sanitizers, short protocol fuzzing, the curl runtime spike on the core leg,
and the host-specific reflection leg. It
runs all available legs and reports host-specific toolchains that are
unavailable locally; hosted CI is authoritative for those environments.
The M4 OpenAI/worker gates and M3 reflection gate are live through that same
preflight entry point.
The M5 showcase gate is also wired through this entry point and passes locally:
it runs 20 deterministic tests three times, compiles and executes a real
headless Dear ImGui frame, and audits the default-OFF package plus a downstream
consumer. Hosted CI runs and passes the same gate.
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
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, coverage and CRAP/complexity gating, static and dynamic analysis (sanitizers, fuzzing, mutation testing), CI shape, workflow, and the absolute-gates philosophy — fixed floors, visible drift. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row with milestone and verification method. When prose elsewhere conflicts with the register, the register wins. |
| [ADR 0001](docs/adr/0001-public-object-graph-and-lifetimes.md) | Accepted public ownership, registry snapshot, Turn detach, and callback-lifetime decisions. |
| [ADR 0002](docs/adr/0002-build-and-dependency-foundation.md) | Build, package, dependency-acquisition, and initial test-harness decisions. |
| [ADR 0003](docs/adr/0003-test-framework-deferred.md) | Historical M0 decision deferring the test framework until M1. |
| [ADR 0004](docs/adr/0004-live-quality-ratchet.md) | Historical merge-base quality ratchet; superseded by ADR 0011. |
| [ADR 0005](docs/adr/0005-m1-runtime-and-test-foundation.md) | Compiled M1 runtime, pinned dependencies, internal contracts, and chat-only milestone boundary. |
| [ADR 0006](docs/adr/0006-m2-agentic-tool-loop.md) | M2 registry snapshots, agentic tool rounds, app-thread dispatch, retry accounting, transactional commit, and Conversation persistence. |
| [ADR 0007](docs/adr/0007-m3-reflection-contract.md) | Accepted M3 schema/type mapping, strict marshalling, description precedence, optional package component, and no-Glaze public boundary. |
| [ADR 0008](docs/adr/0008-m4-openai-compatible-contract.md) | Accepted M4 endpoint, authentication, common request/response, streaming, error, and per-dialect state contract for OpenAI-compatible Chat Completions. |
| [ADR 0009](docs/adr/0009-m4-worker-tool-execution.md) | Accepted M4 per-tool execution policy, handler ownership, ordered control flow, cancellation, observer, and teardown contract. |
| [ADR 0010](docs/adr/0010-m5-showcase-contract.md) | Accepted M5 showcase-only boundary, host-owned ImGui lifecycle, deterministic NPC tools, pinned build-only dependency, and acceptance gates. |
| [ADR 0011](docs/adr/0011-absolute-quality-gates.md) | Absolute quality gates from a single build replace the merge-base ratchet, bespoke reflection coverage validator, nightly mutation schedule, and model manifest pin. |

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.
