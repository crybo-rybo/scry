# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

Scry lets a C++ application send messages to an LLM and give the LLM tools.
Your application keeps control of its main loop. You keep your game loop, GUI
event loop, or simulation tick. Scry runs the full conversation in the
background. It gives you the results when you ask for them.

Write a tool as an ordinary C++ function. Or, give the model a full object, and
each annotated member function of that object is a tool. Scry generates the JSON
schema that the model sees. It checks the arguments that the model sends back,
calls your function, and sends the answer to the model. All work that the model
requests runs on your thread, at a time that you select. Thus, tools can read
and write the state of your application directly.

Scry is pre-1.0. The API, the ABI, and the format of saved conversations can
change.

## What you get

**One call sends a message, one call gets the results.**
`send()` returns immediately. Each time that you call `update()` from your loop,
Scry delivers the text that the model streamed and runs the tools that the model
requested. It also reports when the turn is done. If you give `update()` a time
budget, it stops early so that your frame stays in its time limit. For scripts
and tests, use `send_and_wait()`. It calls `update()` for you until the turn
finishes.

**Tools from ordinary C++.**
Annotate a member function with `[[= scry::reflection::tool{"..."}]]` and
register the object. Each annotated member function becomes a tool that is bound
to the object. Scry registers all of these tools or none of them. You register
free functions and full namespaces of free functions in the same way. You can
also register a lambda that takes an argument struct. Scry uses C++26 reflection
to derive the schema from the parameters, decode the arguments from the model
strictly, and encode the return value. Scry supports nested structs, vectors,
arrays, optionals, enums, and tagged variants. Each parameter description is an
annotation on a struct member. Some tools exist only at runtime, for example a
tool that a script supplies. For such a tool, register a hand-written JSON
schema. Use a handler that takes JSON and returns JSON.

**Scry runs the tool loop.**
When the model calls tools, Scry runs them and sends the results back to the
model. Scry continues until the model gives a final answer. Incorrect arguments,
unknown tools, and handler failures do not cause a crash or abort the turn.
Scry changes each of these into an error message. The model can read this
message and recover.

**Answers as C++ values.**
You can ask for a struct instead of text. `ask<Verdict>()` gives the model a
`respond` tool. Scry generates the schema of this tool from `Verdict`, and it
decodes the answer strictly. If the answer is not correct, Scry sends it back
to the model. The message names the incorrect field. `send<Verdict>()` does the
same work from a main loop. `send_structured()` takes a `ResponseFormat` at
runtime.

**You control what the model can do.**
Each turn can have a hook. The hook sees each tool call before the call runs,
and it can refuse the call with a message that the model reads. Limits on tool
rounds and on tool calls per turn set a maximum size for the loop. You select
what occurs when a turn reaches the round limit. The turn fails, or the turn
completes and gives the calls that did not run back to you.

**Streaming, retries, and cancellation.**
Scry delivers text while the model streams it. Scry retries after network
failures, rate limits, and server errors. It uses exponential backoff and obeys
`Retry-After`. You can cancel a turn at any time. You can also detach a turn.
A detached turn finishes without callbacks.

**History commits a full turn or nothing.**
A conversation records a turn only when the full turn succeeds. Then the
conversation adds the user message, each tool round, and the final reply
together. If a turn fails or is cancelled, the history does not change. You can
save a conversation to JSON and load it from JSON. Thus, you decide where to
keep your conversations.

**Two provider dialects, which the configuration selects.**
Scry supports Anthropic Messages. It also supports the Chat Completions API that
OpenAI-compatible servers, for example Ollama, serve. Scry works with local
servers that have no API key. TLS verification is on by default. You can set a
CA bundle, a proxy, and more headers.

**Errors are values. Limits are explicit.**
Calls that can fail return `std::expected`. One `Config` holds all the limits,
and each limit has a good default value. These limits include byte limits on
payloads, tool arguments, tool results, and conversation size. They also include
the connect, idle, and transfer timeouts.

**Test without a server.**
The optional `scry::testing` library replaces only the HTTP transfer. It uses a
script of prepared responses instead. All other parts, from request encoding to
tool dispatch, are the production code. Thus, your integration tests run the
real runtime without a network.

**Export your tool contract.**
A registry can write a JSON manifest of every registered tool. The manifest
includes the name, description, and schema of each tool. This operation does not
need a model, a network, or libcurl. Thus, you can use it in a build step.

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

// A toolbox: each member function annotated as a tool becomes one the model can
// call, named after the function and bound to this object.
class HostTools {
public:
  [[= scry::reflection::tool{
      "Report whether the host application's main loop is running"}]]
  StatusResult get_application_status(StatusArguments arguments) const {
    return {.running = true, .state = arguments.verbose ? "main loop running" : ""};
  }
};

int main() {
  scry::ToolRegistry tools;
  const auto registered = tools.add(HostTools{});
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

The model reads the schema of the tool, calls the tool, receives the result,
and answers. All work between `send()` and the final callback occurs on a worker
thread. The exceptions are the tool handler and the callbacks. They run inside
`update()`.

The same tool can also be a free function. Register it with
`tools.add<^^get_application_status>()`. Or, it can be one of the tool functions
in a namespace. Register all of them with `tools.add<^^host_tools>()`. A tool
function can also take plain parameters, for example
`(int dx, std::string reason)`. Then Scry builds the argument object from these
parameters. To register a lambda, use
`tools.add<StatusArguments>({.name = ..., .description = ...},
callable)`. To keep a handle on the toolbox that the tools
change, give a `std::shared_ptr` to `add()`.

## How it fits into your application

- **Your thread keeps control.** Each `Harness` has one worker thread that does
  the network I/O. Scry does not call your code until you call `update()`. Then
  your code runs on the calling thread. Tool handlers can read and change game,
  GUI, or simulation state without locks. A slow handler uses more frame time.
  Scry never preempts your thread.
- **Turns are queued in order.** Several conversations can share one `Harness`.
  Each conversation has at most one turn that is queued or in progress. Turns
  run in first-in, first-out order.
- **The core of the loop is deterministic.** The agentic loop is a pure state
  machine. It has no I/O and no clock. Thus, the tests for retries,
  cancellation, and multi-round tool use do not need a network.
- **JSON without a third-party type.** You register dynamic tools with
  `add_dynamic()`. Their handlers read arguments with `scry::JsonView`. They
  build results with `scry::escape_json_string()`. The public headers do not
  expose a parser library.

## Requirements

- **GCC 16 or newer.** You declare tools with C++26 reflection. Thus, the public
  headers need `-std=c++26 -freflection`. Clang and MSVC cannot use the library.
- **CMake 3.30** and **libcurl 7.84** or newer, with development headers. CMake
  3.30 is the first CMake version that knows the C++26 mode of GCC.
- **Linux or macOS.** CI runs GCC 16 on Ubuntu 24.04 and macOS 15.

libcurl is the only library dependency. The code of Scry parses and writes
JSON. The tests also fetch Catch2. For the development toolchain, see
[Contributing](docs/contributing.md).

To run the unit and integration suites, use `./scripts/test.sh`. To run an
end-to-end test with a live model, use `./scripts/test-e2e.sh`. Set the server
URL and the model as
[Contributing](docs/contributing.md#end-to-end-testing) tells you.
`./scripts/ci.sh` runs all local CI checks and the showcase build.

## Install

Build and install from the repository root:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-16 \
  -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
cmake --build build/release
cmake --install build/release --prefix /your/prefix
```

Then, add these lines to a project that you configure with GCC 16 and
`-DCMAKE_PREFIX_PATH=/your/prefix`:

```cmake
find_package(scry 0.6.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

You can also get Scry with `FetchContent`. When you embed Scry, its tests and
examples are off by default:

```cmake
include(FetchContent)
FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.6.0
)
FetchContent_MakeAvailable(scry)
target_link_libraries(app PRIVATE scry::scry)
```

## Learn more

- [examples/main_loop.cpp](examples/main_loop.cpp) — the primary example. It
  shows a reflected tool and a dynamic tool, and it renders the history. Its
  `--tool-manifest` option exports the tool contract without a live model.
- [examples/toolbox.cpp](examples/toolbox.cpp) — all the ways to declare a
  reflected tool: a toolbox class, a namespace of functions, one annotated
  function, and parameters that Scry changes into an argument object.
- [examples/tool_policy.cpp](examples/tool_policy.cpp) — a handler rejects a
  move with a message that the model reads. Then the model tries again.
- [examples/seeded_trials.cpp](examples/seeded_trials.cpp) — the example runs
  the same prompt several times on a local model with a fixed sampling seed.
- [examples/typed_values.cpp](examples/typed_values.cpp) — the reflected codec
  alone. The example shows an annotated answer type, its schema, a strict
  decode, and the error text that Scry sends back to a model.
- [examples/typed_answer.cpp](examples/typed_answer.cpp) — a turn that ends
  with a C++ value. A host validator checks the value.
- [examples/testing_scripted.cpp](examples/testing_scripted.cpp) — a downstream
  test that uses a scripted provider and no network.
- [extras/showcase](extras/showcase) — a standalone Dear ImGui chat panel, and a
  grid world where the model controls an NPC with tools.
- [Architecture](docs/architecture.md) — how Scry is built, what it guarantees,
  and its operation limits. Read this document before you rely on a specific
  behavior.
- [Contributing](docs/contributing.md) — the toolchain configuration, presets,
  gates, and what a change needs before it is merged.
- API reference: `./.github/scripts/ci-docs.sh` writes the Doxygen site to
  `build/docs/html/index.html`.

## License

Scry is released under the MIT License ([LICENSE](LICENSE)).
