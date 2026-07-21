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
development-era coverage/CRAP gating machinery was retired at the release
posture ([ADR 0012](docs/adr/0012-release-infrastructure-simplification.md));
its final full run measured 93.322% diff branch coverage and a maximum CRAP
of 13.125, and the test suites it demanded remain in full.

The scheduled/manual nightly pipeline runs CodeQL, long
SSE/Anthropic/OpenAI fuzzing, and the showcase gate, plus an on-demand
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
repeats the reflection suite under ASan+UBSan. The reflection-OFF install and
consumer remain clean C++23 surfaces with no reflection artifacts.

## Using scry

Scry supports Linux and macOS and requires a C++23 toolchain, CMake ≥ 3.25,
and libcurl ≥ 7.84 development headers. CI covers GCC 14, Clang 18 (with
libc++), and AppleClang on macOS 15. The C++26 reflection component is
optional and requires GCC 16 or newer.

Consume an installed package:

```sh
cmake -S scry -B scry/build -DCMAKE_BUILD_TYPE=Release
cmake --build scry/build
cmake --install scry/build --prefix /your/prefix
```

```cmake
find_package(scry 0.0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

Reflected typed tools are an explicit opt-in on a reflection-enabled install:
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)` and link
`scry::reflection`.

Or vendor it with FetchContent — Scry's tests, examples, and format targets
stay off automatically when it is not the top-level project:

```cmake
include(FetchContent)
FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.0.1
)
FetchContent_MakeAvailable(scry)
target_link_libraries(app PRIVATE scry::scry)
```

The canonical first program is [examples/main_loop.cpp](examples/main_loop.cpp):
create a `Harness` from a `Config`, register a tool, `send()` a message, and
pump `update()` from the loop you already own.

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

That one command adds clang-tidy, the ASan/UBSan and TSan suites, and the
host-specific GCC 16 reflection leg — the same set the hosted per-commit CI
ring enforces ([ADR 0012](docs/adr/0012-release-infrastructure-simplification.md)).
It runs all available legs and reports host-specific toolchains that are
unavailable locally; hosted CI is authoritative for those environments.
Long protocol fuzzing and the M5 showcase gate run in the scheduled nightly
workflow; `just showcase` runs the showcase gate locally.
`just ci` is the optional convenience wrapper.

The reflection-OFF surface targets stable C++23 compilers. The accepted M3
package shape keeps it that way: a reflection-enabled build uses GCC 16+ with
`-std=c++26 -freflection`, and consumers opt in with
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)` and
`scry::reflection`. Core-only builds and installations contain no reflection
component or experimental language flags. The shared reflection build, test,
ASan+UBSan, install-audit, and downstream-consumer gate is live.
clang-p2996 remains manual, non-gating compatibility work, produces no package
artifacts, and is not claimed as M3 verification.

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the five core public concepts, interaction and threading model (with diagrams), implemented explicit-schema and reflected-tool ergonomics, provider abstraction, open questions, and roadmap (M0–M5). **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure, optional consteval codegen and JSON bridge, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, coverage habits, static and dynamic analysis (sanitizers, nightly fuzzing), CI shape, workflow, and the gates-are-behavioral philosophy of the v0.0.1 release posture. |
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
| [ADR 0011](docs/adr/0011-absolute-quality-gates.md) | Historical: absolute quality gates from a single build replaced the merge-base ratchet, bespoke reflection coverage validator, nightly mutation schedule, and model manifest pin. Gating machinery since retired by ADR 0012. |
| [ADR 0012](docs/adr/0012-release-infrastructure-simplification.md) | v0.0.1 release posture: the coverage/CRAP gating machinery, mutation testing, and feasibility spikes are retired; fuzzing and the showcase move to the nightly ring; behavioral gates (matrix, tests, sanitizers, tidy, package audits) remain. |

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.
