# Scry

Scry is a C++26 library that connects an application to a large language model
(LLM). The model can call C++ functions in your application as tools.

Your application keeps control of its main loop. Scry does the network work on
a background thread. Your tools and callbacks run on your thread, and only when
you call `update()`. Thus, a game loop, a GUI event loop, or a simulation tick
can use Scry without locks.

Scry is pre-1.0. The API, the ABI, and the format of saved conversations can
change.

## Quickstart

<!-- examples/quickstart.cpp -->
```cpp
#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <thread>
#include <utility>

// Each annotated member function becomes a tool that is bound to this object. Scry
// generates the JSON schema from the signature and decodes the model's arguments.
class HostTools {
public:
  [[= scry::reflection::tool{"Report the state of one subsystem"}]] std::string
      subsystem_state(std::string name) const {
    return name + " is running";
  }
};

int fail(const scry::Error& error) {
  std::cerr << error.message << '\n';
  return 1;
}

int main() {
  scry::ToolRegistry tools;
  if (auto added = tools.add(HostTools{}); !added)
    return fail(added.error());

  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  auto harness =
      scry::Harness::create({.base_url = "http://127.0.0.1:11434/v1",
                             .model = "qwen3:8b",
                             .dialect = scry::ProviderDialect::openai_compatible},
                            std::move(tools));
  if (!harness)
    return fail(harness.error());

  auto conversation = scry::Conversation::create();
  if (!conversation)
    return fail(conversation.error());

  const auto print = [](scry::Result<scry::Completion> done) {
    if (done)
      std::cout << done->text << '\n';
    else
      fail(done.error());
  };
  auto turn =
      harness->send(*conversation, "Is the renderer running?", {.on_finished = print});
  if (!turn)
    return fail(turn.error());

  while (!turn->finished()) {
    harness->update(); // tool handlers and callbacks run here, on this thread
    std::this_thread::sleep_for(std::chrono::milliseconds{1}); // your frame
  }
}
```

The model reads the tool schema, calls the tool, and answers. The network work
occurs on a worker thread. The tool handler and the callbacks run inside
`update()`, on your thread. This program is
[examples/quickstart.cpp](examples/quickstart.cpp), and CI compiles it.

A tool can also take one argument struct. Annotate each member with
`scry::reflection::description` to describe the parameter to the model. You can
register tools in these ways:

| Tool form | Registration |
| --- | --- |
| Toolbox object | `tools.add(HostTools{})` |
| Toolbox that you keep a handle to | `tools.add(std::make_shared<HostTools>())` |
| Free function | `tools.add<^^subsystem_state>()` |
| All tool functions in a namespace | `tools.add<^^host_tools>()` |
| Lambda that takes an argument struct | `tools.add<Arguments>({.name = ..., .description = ...}, callable)` |

## Features

- **Non-blocking turns.** `send()` returns immediately. Each call to `update()`
  delivers the streamed text, runs the requested tools, and reports when the
  turn is complete. A time budget makes `update()` stop early to protect your
  frame time. For scripts and tests, `send_and_wait()` calls `update()` for you.
- **Tools from C++ functions.** Annotate a member function with
  `[[= scry::reflection::tool{"..."}]]` and register the object. You can also
  register free functions, full namespaces, and lambdas. Scry uses C++26
  reflection to make the JSON schema, decode the arguments strictly, and encode
  the result. Scry supports nested structs, vectors, arrays, optionals, enums,
  and tagged variants.
- **Runtime tools.** For a tool that exists only at runtime, register a JSON
  schema and a handler that takes JSON and returns JSON.
- **Managed tool loop.** Scry runs each tool call and sends the result to the
  model until the model gives a final answer. Incorrect arguments, unknown
  tools, and handler failures do not stop the turn. Scry sends each failure to
  the model as an error message, and the model can recover.
- **Typed answers.** `ask<Verdict>()` makes the model answer with a `Verdict`
  value instead of text. Scry decodes the answer strictly. If a field is
  incorrect, Scry identifies the field to the model and asks again.
  `send<Verdict>()` does the same from a main loop. `send_structured()` takes a
  `ResponseFormat` at runtime.
- **Tool policy.** A hook examines each tool call before the call runs. The
  hook can refuse the call with a message to the model. Limits on tool rounds
  and tool calls set the maximum size of each turn. At the round limit, the
  turn fails or returns the calls that did not run.
- **Streamed output, retries, and cancellation.** Scry delivers text while the
  model streams it. Scry retries after network failures, rate limits, and
  server errors, with exponential backoff that obeys `Retry-After`. You can
  cancel or detach a turn at any time.
- **Atomic history.** A conversation records a turn only when the full turn
  succeeds. If a turn fails or you cancel it, the history does not change. You
  can save a conversation to JSON and load it again.
- **Two provider dialects.** Scry supports the Anthropic Messages API and the
  OpenAI-compatible Chat Completions API, for example Ollama. Local servers do
  not need an API key. TLS verification is on by default, and you can set a CA
  bundle, a proxy, and custom headers.
- **Errors as values, explicit limits.** Calls that can fail return
  `std::expected`. One `Config` holds all limits, and each limit has a safe
  default. These include byte limits on payloads, tool data, and conversation
  size, and the connect, idle, and transfer timeouts.
- **Scripted test provider.** The optional `scry::testing` library supplies a
  scripted HTTP server on loopback. Set `base_url` to this server to test the
  full production code, from libcurl to tool dispatch, without a model.
- **Tool manifest export.** A registry can write a JSON manifest of all
  registered tools. This operation does not need a model, a network, or
  libcurl. Thus, you can use it in a build step.

## Threading model

- **Your thread keeps control.** Each `Harness` has one worker thread for
  network I/O. Scry calls your code only inside `update()`, on the calling
  thread. A slow tool handler uses more frame time, but Scry never preempts
  your thread.
- **Turns run in order.** Many conversations can share one `Harness`. Each
  conversation has a maximum of one queued or active turn. Turns run in
  first-in, first-out order.
- **Deterministic core.** The agentic loop is a pure state machine with no I/O
  and no clock. Thus, the tests for retries, cancellation, and multi-round tool
  use do not need a network.
- **No third-party JSON types.** Dynamic tools from `add_dynamic()` read
  arguments with `scry::JsonView` and make results with
  `scry::escape_json_string()`. The public headers do not expose a parser
  library.

## Requirements

- **GCC 16 or newer.** The public headers use C++26 reflection and need
  `-std=c++26 -freflection`. Scry does not support Clang or MSVC.
- **CMake 3.30 or newer.** This is the first CMake version that knows the C++26
  mode of GCC.
- **libcurl 7.84 or newer**, with development headers. libcurl is the only
  library dependency. Scry parses and writes JSON with its own code.
- **Linux or macOS.** CI uses GCC 16 on Ubuntu 24.04.

## Install

Build and install from the repository root:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-16 \
  -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
cmake --build build/release
cmake --install build/release --prefix /your/prefix
```

In your project, configure with GCC 16 and `-DCMAKE_PREFIX_PATH=/your/prefix`.
Then add these lines:

```cmake
find_package(scry 0.7.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

You can also use `FetchContent`. When you embed Scry, its tests and examples
are off by default:

```cmake
include(FetchContent)
FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.7.0
)
FetchContent_MakeAvailable(scry)
target_link_libraries(app PRIVATE scry::scry)
```

## Tests

To run the unit and integration tests, use `./scripts/test.sh`. The tests
download Catch2. To run the end-to-end tests with a live model, use
`./scripts/test-e2e.sh`. For the server and model settings, refer to
[Contributing](docs/contributing.md#end-to-end-testing).

## Documentation

| Resource | Content |
| --- | --- |
| [examples/main_loop.cpp](examples/main_loop.cpp) | The primary example: a reflected tool, a dynamic tool, and the history. `--tool-manifest` exports the tool contract without a model. |
| [examples/toolbox.cpp](examples/toolbox.cpp) | All the ways to declare a reflected tool. |
| [examples/tool_policy.cpp](examples/tool_policy.cpp) | A handler refuses a move with a message, and the model tries again. |
| [examples/seeded_trials.cpp](examples/seeded_trials.cpp) | The same prompt, many times, on a local model with a fixed sampling seed. |
| [examples/typed_values.cpp](examples/typed_values.cpp) | The reflected codec alone: an answer type, its schema, a strict decode, and the error text. |
| [examples/typed_answer.cpp](examples/typed_answer.cpp) | A turn that ends with a C++ value that a host validator checks. |
| [examples/testing_scripted.cpp](examples/testing_scripted.cpp) | A downstream test with a scripted server on loopback and no model. |
| [extras/showcase](extras/showcase) | A Dear ImGui chat panel, and a grid world where the model controls an NPC with tools. |
| [Architecture](docs/architecture.md) | The design, the guarantees, and the operation limits. Read it before you rely on a specific behavior. |
| [Contributing](docs/contributing.md) | The toolchain, the commands, and the requirements for a change. |
| [include/scry/](include/scry/) | The API reference, in the public headers. |

## License

Scry is released under the MIT License. Refer to [LICENSE](LICENSE).
