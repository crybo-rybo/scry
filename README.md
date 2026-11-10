# Scry

Scry is a C++26 library that runs a conversation between your application and
a large language model (LLM). It sends the request, streams the answer, runs
the C++ functions that the model calls as tools, sends their results back, and
repeats until the model gives its final answer. Then it commits the exchange
to the conversation history.

You declare a tool as a usual C++ function with one annotation. You do not
write JSON: the tool's JSON schema, a strict argument decoder, and a result
encoder come from the declaration by C++26 static reflection, so the schema
always agrees with the code.

Your application keeps control of its main loop. Scry does the network work on
a background thread. Your tools and callbacks run on your thread, and only when
you call `update()`.

## What Scry does

- **Runs the tool loop.** One `send()` covers the whole exchange: the request,
  each round of tool calls, and the final answer. Scry bounds the rounds and
  the calls, and it retries a failed request before any output arrived.
- **Fits a main loop.** `send()` returns at once. Call `update()` from your
  frame with a time budget. Text deltas, tool handlers, and completion
  callbacks run inside it, on the calling thread, so your tools need no locks.
  `send_and_wait()` and `ask()` do the pumping for scripts and tests.
- **Returns typed answers.** `ask<Verdict>()` makes the model answer with a
  C++ struct. A reply of the wrong shape goes back to the model with the
  reason, and the model tries again.
- **Lets you decide which calls run.** A tool can refuse a call with a message
  for the model. A hook can refuse a call before its handler runs. A refusal
  answers the model and never fails the turn.
- **Speaks two protocols.** Anthropic Messages, and OpenAI-compatible Chat
  Completions, which covers local servers such as Ollama. Both stream.
- **Keeps history consistent.** A turn commits its messages together when it
  succeeds. A failed or cancelled turn commits nothing. A conversation saves
  to JSON and restores from it.
- **Is testable without a model.** `scry::testing` is a scripted HTTP server
  on loopback. Your tests run all of Scry against it, with no network.

## Scry and Scrydantic

Scry is built on [Scrydantic](https://github.com/crybo-rybo/scrydantic), a
separate library that turns C++ declarations into JSON. A program that uses
Scry uses both, and each has its own namespace:

| | Scrydantic (`scrydantic::`) | Scry (`scry::`) |
|---|---|---|
| Does | Turns a struct into a JSON schema, a strict decoder, and an encoder. Turns an annotated function into a tool handler. Parses and writes JSON. | Registers tools, talks to the model, runs the tool loop, delivers events to your thread, and keeps the conversation. |
| You write | The annotations (`tool`, `description`, `range`, ...), `Json`, and what a tool returns (`Result<T>`, `Status`, `tool_error()`). | `ToolRegistry`, `Harness`, `Conversation`, `Config`, the turn callbacks, and `Error`. |
| Documented in | The [Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md): supported types, annotations, constraints, validators, and decode errors. | The [Scry guide](docs/guide.md) and [architecture](docs/architecture.md). |

`<scry/scry.hpp>` includes Scrydantic, and the `scry::scry` CMake target links
it, so you add nothing more. Scrydantic needs neither Scry nor a network. You
can use it alone, for example to serve the same tools over MCP.

## Quickstart

<!-- examples/quickstart.cpp -->
```cpp
#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <thread>
#include <utility>

// Each annotated member function becomes a tool that is bound to this object. The
// JSON schema and the argument decoder come from the signature, by reflection.
class HostTools {
public:
  [[= scrydantic::tool{"Report the state of one subsystem"}]] std::string
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

The model reads the tool schema, calls `subsystem_state`, and answers. The
words depend on the model. The program names `qwen3:8b`, and any model that
supports tools works. This output is from a run with `qwen2.5:7b`:

```
The renderer is currently running.
```

This program is [examples/quickstart.cpp](examples/quickstart.cpp). A test
checks that the block above is the file, and CI compiles it.

## One declaration makes the full tool

```cpp
struct WaterArguments {
  [[= scrydantic::description{"Bed number"}, = scrydantic::range{.min = 1, .max = 3}]]
  std::int32_t bed;
  [[= scrydantic::description{"Liters to pour"},
    = scrydantic::range{.exclusive_min = 0.0}]] double liters{1.0};
};

[[= scrydantic::tool{"Water one bed"}]] scrydantic::Status
    water(WaterArguments arguments);

scry::ToolRegistry tools;
tools.add<^^water>(); // returns scry::Status: examine it
```

Scrydantic makes this schema from the declaration, and Scry sends it to the
model under the tool name `water`:

```json
{
  "additionalProperties": false,
  "properties": {
    "bed": {
      "description": "Bed number",
      "maximum": 3,
      "minimum": 1,
      "type": "integer"
    },
    "liters": {
      "description": "Liters to pour",
      "exclusiveMinimum": 0,
      "type": "number"
    }
  },
  "required": ["bed"],
  "type": "object"
}
```

When the model calls `water`, the same declaration decodes its arguments.
Three things follow for your application:

- **Your function gets only valid arguments.** A missing member, an incorrect
  type, a value past a constraint, and an unknown member are all rejected
  before the call.
- **The model can correct itself.** Scry sends each problem back as the tool
  result, one line for each, with the path of the field: `$.bed must be at
  most 3`. The turn continues, and the model calls again.
- **A type that has no JSON form does not compile.** The diagnostic gives the
  member and the reason.

The [Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md)
lists the supported types and every annotation. To print the schema of each
tool in a program, with no model and no network, call
`ToolRegistry::to_json()`, or run [examples/toolbox.cpp](examples/toolbox.cpp)
with `--tool-manifest`.

## An answer as a C++ value

```cpp
struct Verdict {
  [[= scrydantic::description{"Is the claim supported by evidence?"}]] bool supported{};
  [[= scrydantic::description{"One sentence explaining the verdict"}]] std::string reason{};
};

auto verdict = harness->ask<Verdict>(*conversation, "Is the moon made of cheese?");
if (verdict)
  std::cout << verdict->value.supported << ' ' << verdict->value.reason << '\n';
```

The model can still call your tools before it answers.
[examples/typed_answer.cpp](examples/typed_answer.cpp) is the full program.

## Requirements

- **GCC 16.2 or newer.** The public headers use C++26 reflection and need
  `-std=c++26 -freflection`. Scry does not support Clang or MSVC. The official
  `gcc:16.2.0` container image has this compiler. Ubuntu's toolchain PPA
  packages only development snapshots of GCC 16, which do not qualify.
- **CMake 3.30 or newer.** 3.30 is the first CMake version that knows the
  C++26 mode of GCC.
- **libcurl 7.84 or newer**, with development headers.
- **Scrydantic 0.2.x.** CMake uses an installed package when one is
  available. Otherwise it fetches a pinned revision.
- **Linux or macOS.**

## Install

Build and install from the repository root:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-16 \
  -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
cmake --build build/release
cmake --install build/release --prefix /your/prefix
```

In your project, configure with GCC 16.2 or newer and
`-DCMAKE_PREFIX_PATH=/your/prefix`.
Then add these lines:

```cmake
find_package(scry 0.7.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

`scry::scry` supplies the headers, the C++26 reflection flags, and Scrydantic
transitively. When CMake fetched Scrydantic, the install includes its package
alongside Scry. When you use an installed Scrydantic, keep its prefix on
`CMAKE_PREFIX_PATH` for your project too.

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

For the scripted test server, request the `testing` component and link
`scry::testing` to your tests:

```cmake
find_package(scry 0.7.0 CONFIG REQUIRED COMPONENTS testing)
target_link_libraries(app_tests PRIVATE scry::scry scry::testing)
```

| Option | Default | Purpose |
|---|---|---|
| `SCRY_BUILD_TESTING_SUPPORT` | On | Build and install `scry::testing` |
| `SCRY_FETCH_SCRYDANTIC` | On | Fetch the pinned Scrydantic when no installed package or parent target exists |
| `SCRY_BUILD_TESTS` | On at top level | Build Scry's own tests |
| `SCRY_BUILD_EXAMPLES` | On at top level | Compile the programs under `examples/` |
| `SCRY_WARNINGS_AS_ERRORS` | On at top level | Treat warnings in Scry's own sources as errors |
| `SCRY_SANITIZER` | `none` | `none`, `address-undefined`, or `thread`. Also instruments a fetched Scrydantic |

To require an installed Scrydantic and prevent a download, set
`-DSCRY_FETCH_SCRYDANTIC=OFF` and set `CMAKE_PREFIX_PATH` or `scrydantic_DIR`.
A parent project can also supply the `scrydantic::scrydantic` and
`scrydantic::json` targets before it adds Scry.

## Documentation

| Resource | Content |
|---|---|
| [docs/guide.md](docs/guide.md) | How to use Scry, by task: tools, typed answers, the main loop, providers, errors, persistence, and tests |
| [docs/architecture.md](docs/architecture.md) | What Scry guarantees and where its limits are. Read it before you rely on a specific behavior |
| [include/scry/](include/scry/) | The API reference, in the public headers |
| [Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md) | Types, annotations, constraints, validators, schemas, and decode errors |
| [docs/development.md](docs/development.md) | How to build, test, and change Scry itself |

## Examples

Each program is under [examples/](examples/) and compiles with the library.
Those that talk to a model assume an OpenAI-compatible server on
`127.0.0.1:11434`, such as `ollama serve`.

| Program | Shows | Needs a model |
|---|---|---|
| [quickstart.cpp](examples/quickstart.cpp) | One tool, one turn, and a polling loop | Yes |
| [toolbox.cpp](examples/toolbox.cpp) | Each way to register a reflected tool, and the tool manifest | Only without `--tool-manifest` |
| [tool_policy.cpp](examples/tool_policy.cpp) | A tool refuses a call with a message, and the model tries again | Yes |
| [main_loop.cpp](examples/main_loop.cpp) | A frame loop with an update budget, a dynamic tool, an admission hook, and the committed history | Yes |
| [typed_answer.cpp](examples/typed_answer.cpp) | A turn that ends with a C++ value, and one with a hand-written response format | Yes |
| [seeded_trials.cpp](examples/seeded_trials.cpp) | The same prompt, many times, with a fixed sampling seed | Yes |
| [testing_scripted.cpp](examples/testing_scripted.cpp) | A test of an application against the scripted server | No |
| [typed_values.cpp](examples/typed_values.cpp) | Scrydantic alone: a schema, a strict decode, and the text a model gets for an incorrect value | No |

[extras/showcase](extras/showcase) has two larger programs: a Dear ImGui chat
panel, and a grid world where the model controls an NPC with tools.

## License

Scry is released under the MIT License. Refer to [LICENSE](LICENSE).
