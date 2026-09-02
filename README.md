# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. It hides the full
agentic loop — HTTP, streaming, tool dispatch, retries — behind a small,
poll-friendly async API with an explicitly named synchronous convenience. Tools
register either with an explicit JSON schema or, through C++26 reflection
(P2996), straight from a plain struct; both lower onto the same runtime
registry. The name is the design: reflection (the mirror) + consulting an
oracle (the LLM).

Built for the apps that live in C++ — games, GUI tools, simulators — where you can't block a frame, can't shell out to Python, and want tool use, not just chat.

## Highlights

- **The complete agentic tool loop, owned by the library.** One `send()` covers
  the model request, streamed output, tool dispatch, automatic resend of tool
  results, and the final answer — with cancellation and callback disconnection
  as separate controls, bounded retries, and transactional conversation
  history.
- **Fits the loop you already own.** `send()` never waits on network I/O, and
  every callback and tool handler runs inside the `update()` you call from your
  own main loop, on your own thread. No event loop is imposed and no locks are
  required in application code.
- **Two provider dialects from `Config` alone.** Anthropic Messages, plus a
  strict OpenAI-compatible Chat Completions subset that also drives local
  servers such as Ollama, vLLM, and llama.cpp server with no API key. Optional
  typed reasoning disablement leaves the portable request unchanged by default.
- **Typed tools, two ways.** Register a tool with an explicit JSON schema, or
  let C++26 reflection derive the schema and the argument marshalling from a
  plain struct. Both lower onto the same registry, and both are part of the
  library's one target.
- **JSON and history you can read without a parser.** `scry::JsonView` reads
  the Scry-owned `Json` boundary type — kind, scalar accessors, array index,
  object lookup, ordered keys — and `scry::escape_json_string()` writes one by
  hand. `Conversation::messages()` exposes committed history as the public
  message model, so a chat UI renders state instead of mirroring it.

Scry is pre-1.0: no API or ABI stability is promised yet.

## Requirements

Scry supports Linux and macOS and requires **GCC 16 or newer**, CMake ≥ 3.25,
and libcurl ≥ 7.84 development headers. C++26 reflection (P2996) is part of the
core public API, and GCC 16 is currently the only compiler that implements it,
so Clang is not yet supported for consumers. CI covers GCC 16 on Ubuntu 24.04
(from `ppa:ubuntu-toolchain-r/test`) and on macOS 15 (from Homebrew).

## Installation

### As an installed package

```sh
cmake -S scry -B scry/build -DCMAKE_BUILD_TYPE=Release
cmake --build scry/build
cmake --install scry/build --prefix /your/prefix
```

```cmake
find_package(scry 0.2.0 CONFIG REQUIRED)
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
  GIT_TAG v0.2.0
)
FetchContent_MakeAvailable(scry)
target_link_libraries(app PRIVATE scry::scry)
```

## Getting started

[examples/main_loop.cpp](examples/main_loop.cpp) is the one example, and it is
the canonical first program: create a `Harness` from a `Config`, register one
reflected tool and one explicit-schema tool, `send()` a message with the
callbacks you want, and pump `update()` from the loop you already own. It assumes
a local Ollama server at `http://127.0.0.1:11434/v1` with the `qwen3:8b` model
installed.

`scry::reflection::encode(value)` converts any supported reflected value to
canonical Scry-owned `Json` without registering a tool.

[Public API design](docs/design/public-api.md) walks through the same five public concepts —
`Config`, `Conversation`, `ToolRegistry`, `Turn`, `Harness` — with a complete
annotated integration.

## Documentation

| Document | Contents |
|---|---|
| [Design](docs/design/overview.md) | Vision, goals/non-goals, public concepts, runtime behavior, tool ergonomics, provider abstraction, and forward scope. **Start here.** |
| [Architecture](docs/architecture/overview.md) | Actor-model concurrency, the sans-I/O state machine, type erasure, providers and transport, dependencies, and the evolution register. |
| [Development](docs/development/principles-and-testing.md) | Testing strategy, quality gates, static and dynamic analysis, CI shape, workflow, and the definition of done. |
| [Requirements](docs/requirements.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row. When prose elsewhere conflicts with the register, the register wins. |

Recommended reading order: product design → software architecture → development
and quality, then requirements as the binding summary. The first three explain
*why*; the register states *what holds*.

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

That one command adds the Doxygen site, clang-tidy, the ASan/UBSan and TSan
suites, and the fuzz corpus replay. It runs all available legs and reports host-specific toolchains that are
unavailable locally; hosted CI is authoritative for those environments. Long
protocol fuzzing, deep static analysis, and the showcase gate run in the
scheduled weekly workflow; `just showcase` runs the showcase gate locally.
`just ci` is the optional convenience wrapper.
[Development documentation](docs/development/principles-and-testing.md)
describes the full quality machinery and the definition of done.

## License

Scry is released under the MIT License ([LICENSE](LICENSE)).
