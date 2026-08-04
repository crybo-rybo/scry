# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. The stable C++23
surface supports explicit-schema tools and hides the full agentic loop — HTTP,
streaming, tool dispatch, retries — behind a small, poll-friendly async API
with an explicitly named synchronous convenience. C++26 reflection (P2996) is
an isolated optional component that lowers typed tools onto that same runtime
registry. The name is the design: reflection (the mirror) +
consulting an oracle (the LLM).

Built for the apps that live in C++ — games, GUI tools, simulators — where you can't block a frame, can't shell out to Python, and want tool use, not just chat.

## Highlights

- **The complete agentic tool loop, owned by the library.** One `send()` covers
  the model request, streamed output, tool dispatch, automatic resend of tool
  results, and the final answer — with cancellation, bounded retries, and
  transactional conversation history.
- **Fits the loop you already own.** `send()` never waits on network I/O, and
  every callback and tool handler runs inside the `update()` you call from your
  own main loop, on your own thread. No event loop is imposed and no locks are
  required in application code.
- **Two provider dialects from `Config` alone.** Anthropic Messages, plus a
  strict OpenAI-compatible Chat Completions subset that also drives local
  servers such as Ollama, vLLM, and llama.cpp server with no API key.
- **Typed tools, optionally.** The stable surface registers explicit-schema
  tools; an optional, experimental C++26 reflection component (GCC 16 or
  newer) derives schemas and argument marshalling from plain structs and lowers
  onto the same registry.

Scry is pre-1.0: no API or ABI stability is promised yet, and breaking changes
land with a notice in [CHANGELOG.md](CHANGELOG.md).

## Requirements

Scry supports Linux and macOS and requires a C++23 toolchain, CMake ≥ 3.25,
and libcurl ≥ 7.84 development headers. CI covers GCC 14, Clang 18 (with
libc++), and AppleClang on macOS 15. The C++26 reflection component is
optional and requires GCC 16 or newer.

## Installation

### As an installed package

```sh
cmake -S scry -B scry/build -DCMAKE_BUILD_TYPE=Release
cmake --build scry/build
cmake --install scry/build --prefix /your/prefix
```

```cmake
find_package(scry 0.0.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

### With FetchContent

Or vendor it — Scry's tests, examples, and format targets stay off
automatically when it is not the top-level project:

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

### Reflection component

The core `scry::scry` target is plain C++23 on stable compilers; core-only
builds and installations contain no reflection component and no experimental
language flags. Reflected typed tools are an explicit opt-in on a
reflection-enabled install (GCC 16+, `-std=c++26 -freflection`):

```cmake
find_package(scry CONFIG REQUIRED COMPONENTS reflection)
target_link_libraries(app PRIVATE scry::reflection)
```

## Getting started

The canonical first program is [examples/main_loop.cpp](examples/main_loop.cpp):
create a `Harness` from a `Config`, register a tool, `send()` a message with the
callbacks you want, and pump `update()` from the loop you already own. It assumes
a local Ollama server at `http://127.0.0.1:11434` with the `qwen3:1.7b` model
installed.

[DESIGN.md §4](DESIGN.md) walks through the same five public concepts —
`Config`, `Conversation`, `ToolRegistry`, `Turn`, `Harness` — with a complete
annotated integration.

## Diagnostics

Internal diagnostics are opt-in at build time: configure with
`-DSCRY_ENABLE_LOGGING=ON` (preset `dev-logging`) to compile the internal
lifecycle logging, then set the `SCRY_LOG_FILE` environment variable to the
nonempty destination path. With that variable unset or empty, logging stays
disabled and no default file is written. Prompt and tool content and
credentials never reach the log.

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the five core public concepts, interaction and threading model (with diagrams), explicit-schema and reflected-tool ergonomics, provider abstraction, future directions, and forward scope. **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure, optional consteval codegen and JSON bridge, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How the project is engineered: testing plan and pyramid, complexity limits, static and dynamic analysis, CI shape, workflow, and the gates-are-behavioral philosophy of the v0.0.1 release posture. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row. When prose elsewhere conflicts with the register, the register wins. |

Recommended reading order: DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then
REQUIREMENTS.md as the binding summary. The first three explain *why*; the
register states *what holds*.

Accepted architecture decisions live in [docs/adr/](docs/adr/), newest last;
each records the context, decision, and consequences behind one fork in the road.

### API reference

Build the warning-clean API reference with Doxygen 1.9.8 or newer and Graphviz:

```sh
./scripts/ci-docs.sh
```

The styled HTML site is written to `build/docs/html/index.html`. Hosted CI runs
the same command for pull requests and every push to `main`, then retains the
site as the `scry-api-docs` artifact. These documentation tools are build-only
and never enter Scry's installed or exported package surface.

## Developing Scry

To work on Scry itself, run the fast, platform-stable core workflow:

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
host-specific GCC 16 reflection leg. It runs all available legs and reports
host-specific toolchains that are unavailable locally; hosted CI is
authoritative for those environments. Long protocol fuzzing, deep static
analysis, the reflection leg, and the showcase gate run in the scheduled weekly
workflow; `just showcase` runs the showcase gate locally. `just ci` is the
optional convenience wrapper. [ENGINEERING.md](ENGINEERING.md) describes the
full quality machinery and the definition of done.

## License

Scry is released under the MIT License ([LICENSE](LICENSE)). Third-party
dependency licenses are recorded in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
