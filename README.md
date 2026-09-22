# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

Scry lets a C++ application talk to an LLM and give it tools, without handing
over the main loop. You keep your game loop, GUI event loop, or simulation tick.
Scry runs the whole conversation in the background and hands you the results
when you ask for them.

Write a tool as a plain C++ function that takes a struct and returns a struct.
Scry generates the JSON schema the model sees, checks the arguments the model
sends back, calls your function, and sends the answer to the model. Everything
the model asks for runs on your thread, at a moment you choose, so tools can
read and write your application's state directly.

Scry is pre-1.0. The API, ABI, and saved-conversation format may change.

## What you get

**One call sends a message, one call collects the results.**
`send()` returns immediately. Each time you call `update()` from your loop,
Scry delivers streamed text, runs any tools the model requested, and reports
when the turn is done. Give `update()` a time budget and it stops early so your
frame stays on schedule. For scripts and tests, `send_and_wait()` does the
pumping for you.

**Tools from ordinary structs.**
Declare arguments and results as aggregates. Scry derives the schema, decodes
the model's arguments strictly, and encodes the return value, using C++26
reflection. Nested structs, vectors, arrays, optionals, and enums are supported.
Parameter descriptions are annotations on the members. If you already have a
JSON schema, register that instead with a handler that takes and returns JSON.

**The tool loop is handled for you.**
When the model calls tools, Scry runs them, sends the results back, and keeps
going until the model produces a final answer. Bad arguments, unknown tools, and
handler failures become error messages the model can read and recover from,
rather than crashes or aborted turns.

**You decide what the model may do.**
A per-turn hook sees every tool call before it runs and can refuse it with a
message the model reads. Caps on tool rounds and tool calls per turn bound the
loop, and you choose whether hitting the round cap fails the turn or ends it
cleanly with the unexecuted calls handed back to you.

**Streaming, retries, and cancellation.**
Text arrives as it streams. Network failures, rate limits, and server errors are
retried with exponential backoff and honor `Retry-After`. A turn can be
cancelled at any time, or detached so it finishes quietly without callbacks.

**History that never half-commits.**
A conversation records a turn only when the whole thing succeeds: the user
message, every tool round, and the final reply land together. A failed or
cancelled turn leaves history untouched. Conversations save to and load from
JSON, so you own where they are stored.

**Two provider dialects, selected by configuration.**
Anthropic Messages, and the Chat Completions API that OpenAI-compatible
servers such as Ollama serve. Local servers with no API key work.
TLS verification is on by default, with settings for a CA bundle, a proxy, and
extra headers.

**Errors are values, limits are explicit.**
Fallible calls return `std::expected`. Byte limits on payloads, tool arguments,
tool results, and conversation size, plus connect, idle, and transfer timeouts,
all live in one `Config` with sensible defaults.

**Test without a server.**
The optional `scry::testing` library replaces only the HTTP transfer with a
script of canned responses. Everything else, from request encoding to tool
dispatch, is the shipping code, so your integration tests exercise the real
runtime with no network.

**Export your tool contract.**
A registry can write a JSON manifest of every registered tool, name, description,
and schema. It needs no model, no network, and no libcurl, so it fits in a build
step.

## A complete program

```cpp
#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <thread>
#include <utility>

// Scry generates the schema, argument decoding, and result encoding from these
// two structs. The annotation becomes the parameter's description.
struct StatusArguments {
  [[= scry::reflection::description{
      "Include a human-readable state label in the result"}]] bool verbose{false};
};

struct StatusResult {
  bool running{};
  std::string state{};
};

int main() {
  scry::ToolRegistry tools;
  const auto registered = scry::reflection::add<StatusArguments>(
      tools,
      {.name = "get_application_status",
       .description = "Report whether the host application's main loop is running"},
      [](StatusArguments arguments) {
        return StatusResult{.running = true,
                            .state = arguments.verbose ? "main loop running" : ""};
      });
  if (!registered) { std::cerr << registered.error().message << '\n'; return 1; }

  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  auto harness = scry::Harness::create(
      {.base_url = "http://127.0.0.1:11434/v1",
       .model = "qwen3:8b",
       .dialect = scry::ProviderDialect::openai_compatible},
      std::move(tools));
  if (!harness) { std::cerr << harness.error().message << '\n'; return 1; }

  auto conversation = scry::Conversation::create();
  auto turn = harness->send(
      *conversation, "Is the host application main loop running?",
      {.on_finished = [](scry::Result<scry::Completion> finished) {
         if (finished) { std::cout << finished->text << '\n'; }
         else { std::cerr << finished.error().message << '\n'; }
       }});
  if (!turn) { std::cerr << turn.error().message << '\n'; return 1; }

  while (!turn->finished()) {
    harness->update(); // callbacks and tool handlers run on this thread
    std::this_thread::sleep_for(std::chrono::milliseconds{1}); // your frame here
  }
  return 0;
}
```

The model reads the tool's schema, calls it, receives the result, and answers.
Everything between `send()` and the final callback happens on a worker thread,
except the tool handler and the callbacks, which run inside `update()`.

## How it fits into your application

- **Your thread stays in charge.** One worker thread per `Harness` does the
  network I/O. Nothing reaches your code until you call `update()`, and then it
  runs on the calling thread. Tool handlers can touch game, GUI, or simulation
  state without locks. A slow handler costs frame time; Scry never preempts you.
- **Turns are queued in order.** Several conversations can share one `Harness`.
  Each conversation has at most one turn in flight, and turns run first in,
  first out.
- **The loop is deterministic underneath.** The agentic loop is a pure state
  machine with no I/O and no clock, so retries, cancellation, and multi-round
  tool use are tested without a network.
- **JSON without a third-party type.** Explicit-schema handlers read arguments
  through `scry::JsonView` and build results with `scry::escape_json_string()`.
  No parser library is exposed in the public headers.

## Requirements

- **GCC 16 or newer.** Tools are declared with C++26 reflection, so the public
  headers need `-std=c++26 -freflection`. Clang and MSVC cannot consume the
  library.
- **CMake 3.28** and **libcurl 7.84** or newer, with development headers.
- **Linux or macOS.** CI runs GCC 16 on Ubuntu 24.04 and macOS 15.

Glaze is a private header-only dependency that CMake finds or fetches. Tests
additionally fetch Catch2. See [Contributing](docs/contributing.md) for the
development toolchain.

## Install

Build and install from the repository root:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-16 \
  -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
cmake --build build/release
cmake --install build/release --prefix /your/prefix
```

Then, in a project configured with GCC 16 and
`-DCMAKE_PREFIX_PATH=/your/prefix`:

```cmake
find_package(scry 0.4.1 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

Or pull it in with `FetchContent`. Scry's tests and examples default to off
when embedded:

```cmake
include(FetchContent)
FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.4.1
)
FetchContent_MakeAvailable(scry)
target_link_libraries(app PRIVATE scry::scry)
```

## Learn more

- [examples/main_loop.cpp](examples/main_loop.cpp) — the canonical example:
  both tool registration paths, a rendered history, and `--tool-manifest` to
  export the tool contract without a running model.
- [examples/tool_policy.cpp](examples/tool_policy.cpp) — a handler that
  rejects a move with a message the model reads, so the model tries again.
- [examples/testing_scripted.cpp](examples/testing_scripted.cpp) — a downstream
  test with a scripted provider and no network.
- [extras/showcase](extras/showcase) — a standalone Dear ImGui chat panel and a
  grid world where the model drives an NPC through tools.
- [Architecture](docs/architecture.md) — how it is built, what it guarantees,
  and its operating limits. Read this before relying on a specific behavior.
- [Contributing](docs/contributing.md) — toolchain setup, presets, gates, and
  what a change needs before it lands.
- API reference: `./scripts/ci-docs.sh` writes the Doxygen site to
  `build/docs/html/index.html`. Successful non-pull-request runs on `main`
  deploy the current reference to
  [crybo-rybo.github.io/scry](https://crybo-rybo.github.io/scry/).

## License

Scry is released under the MIT License ([LICENSE](LICENSE)).
