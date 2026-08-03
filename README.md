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

**Status:** Scry runs the streaming agentic tool loop end to end — model
request, tool dispatch, automatic resend of results, final answer — with
cancellation, bounded retries, and transactional conversation history. Two
provider dialects are selected from `Config` alone: Anthropic Messages and a
strict OpenAI-compatible Chat Completions subset that also drives local servers
such as Ollama, vLLM, and llama.cpp server with no API key. The core is C++23
and poll-friendly: `send()` never waits on network I/O, and every callback and
tool handler runs inside the `update()` you call from the loop you already own.
The reflected typed-tool layer is an optional, experimental component requiring
GCC 16 or newer; this is a pre-1.0 release, so no API or ABI stability is
promised and breaking changes land with a changelog notice.

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
create a `Harness` from a `Config`, register a tool, `send()` a message with the
callbacks you want, and pump `update()` from the loop you already own. It assumes
a local Ollama server at `http://127.0.0.1:11434` with the `qwen3:1.7b` model
installed.

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
host-specific GCC 16 reflection leg. It runs all available legs and reports
host-specific toolchains that are unavailable locally; hosted CI is
authoritative for those environments. Long protocol fuzzing, deep static
analysis, the reflection leg, and the showcase gate run in the scheduled weekly
workflow; `just showcase` runs the showcase gate locally. `just ci` is the
optional convenience wrapper.

Internal diagnostics are opt-in at build time: configure with
`-DSCRY_ENABLE_LOGGING=ON` (preset `dev-logging`) to compile the internal
lifecycle logging, then set the `SCRY_LOG_FILE` environment variable to the
destination path. With that variable unset, logging stays disabled and no file
is written.

Build the warning-clean API reference with Doxygen 1.9.8 or newer and Graphviz:

```sh
./scripts/ci-docs.sh
```

The styled HTML site is written to `build/docs/html/index.html`. Hosted CI runs
the same command for pull requests and every push to `main`, then retains the
site as the `scry-api-docs` artifact. These documentation tools are build-only
and never enter Scry's installed or exported package surface.

The reflection-OFF surface targets stable C++23 compilers, and the package
shape keeps it that way: a reflection-enabled build uses GCC 16+ with
`-std=c++26 -freflection`, and consumers opt in with
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)` and
`scry::reflection`. Core-only builds and installations contain no reflection
component or experimental language flags. clang-p2996 remains manual,
non-gating compatibility work and produces no package artifacts.

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the five core public concepts, interaction and threading model (with diagrams), explicit-schema and reflected-tool ergonomics, provider abstraction, open questions, and forward scope. **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure, optional consteval codegen and JSON bridge, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, complexity limits, static and dynamic analysis, CI shape, workflow, and the gates-are-behavioral philosophy of the v0.0.1 release posture. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row. When prose elsewhere conflicts with the register, the register wins. |

Accepted architecture decisions live in [docs/adr/](docs/adr/), newest last;
each records the context, decision, and consequences behind one fork in the road.

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.

## License

Scry is released under the MIT License ([LICENSE](LICENSE)). Third-party
dependency licenses are recorded in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
