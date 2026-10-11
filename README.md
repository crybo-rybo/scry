# Scry

Scry is a C++26 library that connects an application to a large language model
(LLM). The model can call C++ functions in your application as tools.

You write a usual C++ function and add one annotation. At compile time, Scry
uses C++26 static reflection to read the declaration. From the declaration,
Scry makes the tool name, the JSON schema, a strict argument decoder, and a
result encoder. You do not write JSON, and the schema always agrees with the
code.

Your application keeps control of its main loop. Scry does the network work on
a background thread. Your tools and callbacks run on your thread, and only when
you call `update()`.

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

The model reads the tool schema, calls the tool, and answers. This program is
[examples/quickstart.cpp](examples/quickstart.cpp), and CI compiles it.

## Tools from C++ declarations

### One declaration makes the full tool

```cpp
struct WaterArguments {
  [[= scry::reflection::description{"Bed number, from 1 to 3"}]] std::int32_t bed;
  [[= scry::reflection::description{"Litres to pour"}]] double litres{1.0};
};

[[= scry::reflection::tool{"Water one bed"}]] scry::Status
    water(WaterArguments arguments);
```

Scry makes this schema from the declaration. The model gets it with the tool
name `water`:

```json
{
  "type": "object",
  "properties": {
    "bed": {
      "type": "integer",
      "description": "Bed number, from 1 to 3",
      "minimum": -2147483648,
      "maximum": 2147483647
    },
    "litres": { "type": "number", "description": "Litres to pour" }
  },
  "required": ["bed"],
  "additionalProperties": false
}
```

- **The types set the schema.** `std::int32_t` gives an integer with the range
  of the type. `litres` has a default value, thus it is not required.
- **The same types decode the arguments.** Scry rejects a missing required
  member, an incorrect type, a value out of range, and an unknown member. The model gets an
  error message that identifies the field, and the turn continues. Your
  function gets only valid arguments.
- **Errors occur at compile time.** If a declaration uses a type that Scry
  cannot encode, the compiler stops. The diagnostic gives the member and the
  reason.

Scry supports `bool`, integers, floating-point types, `std::string`, enums (as
strings), nested structs, `std::vector`, `std::array`, `std::optional`, and
tagged `std::variant`.

### Ways to package tools

Each form is optional. Use the form that agrees with your code, or use many
forms in one registry. All forms, except `add_dynamic()`, get the schema from
reflection.

| Form | Registration | Use it when |
| --- | --- | --- |
| Free function | `tools.add<^^to_fahrenheit>()` | One tool that has no state. |
| Namespace | `tools.add<^^almanac>()` | A group of tools that have no state. Each annotated function declared in the namespace before the call becomes a tool. |
| Toolbox object | `tools.add(Greenhouse{})` | Tools that share state. Each annotated member function becomes a tool bound to the object. The registry owns the object. |
| Shared toolbox | `tools.add(std::make_shared<Greenhouse>())` | Tools that share state, and your application also reads that state. |
| Lambda | `tools.add<WaterArguments>({.name = "water", .description = "Water one bed"}, handler)` | The name or description is known only at runtime, or the handler captures local data. Reflection still makes the schema from the struct. |
| Dynamic | `tools.add_dynamic(definition, handler)` | The tool exists only at runtime, for example a tool from a script. You write the JSON schema and read the JSON arguments. |

The first four forms in one program:

```cpp
// A free function. The name annotation changes the tool name for the model.
[[= scry::reflection::tool{"Convert Celsius to Fahrenheit"},
  = scry::reflection::name{"celsius_to_fahrenheit"}]]
double to_fahrenheit(double celsius);

// A namespace. Each annotated function is a tool.
namespace almanac {
[[= scry::reflection::tool{"Planting advice for one crop"}]]
std::string advice(Crop crop);
}

// A toolbox. Each annotated member function is a tool bound to one object.
class Greenhouse {
public:
  [[= scry::reflection::tool{"Read the temperature and the roof vents"}]]
  Climate climate() const;
  [[= scry::reflection::tool{"Open or close the roof vents"}]]
  Climate set_vents(bool open);
  [[= scry::reflection::tool{"Water one bed"}]]
  scry::Status water(WaterArguments arguments);

  double litres() const; // no annotation, thus not a tool
};

auto house = std::make_shared<Greenhouse>();
scry::ToolRegistry tools;
tools.add(house);               // each add() returns scry::Status: examine it
tools.add<^^almanac>();
tools.add<^^to_fahrenheit>();
```

Each `add()` registers all of its tools or none of them. The full program is
[examples/toolbox.cpp](examples/toolbox.cpp). Run it with `--tool-manifest` to
print each generated schema. This does not need a model or a network.

### Parameters and results

A tool function can have one of these parameter lists:

- **No parameters.** The tool takes `{}`.
- **Plain parameters.** Scry makes an argument object with one required member
  for each parameter. `set_vents(bool open)` takes `{"open": true}`.
- **One struct.** The struct is the argument object. Use this form for
  descriptions, default values, and optional members.

Before these parameters, a tool can take `const scry::ToolCallContext&`. The
context gives the turn, the round, and the call ID.

A tool can return a supported value, a `scry::Result` of one, `void`, or
`scry::Status`. To refuse a call, return
`std::unexpected(scry::tool_error("text for the model"))`. The model reads the
text and can try again. Refer to
[examples/tool_policy.cpp](examples/tool_policy.cpp).

### Annotations

| Annotation | Applies to | Effect |
| --- | --- | --- |
| `tool{"..."}` | Function | Makes the function a tool. The text is the tool description. |
| `name{"..."}` | Function or data member | Changes the tool name or the JSON key. |
| `description{"..."}` | Data member | Describes the parameter to the model. |
| `tag{"..."}` | Class | Identifies a `std::variant` alternative, under the key `"type"`. |
| `skip_null` | Class or data member | Omits an empty `std::optional` instead of writing `null`. |
| `emit_null` | Data member | Writes `null` for one `std::optional` member of a `skip_null` class. |
| `ignore_unknown` | Class | Decoding ignores JSON members that the class does not declare. |

All annotations are in the `scry::reflection` namespace.

### Typed answers

The same reflection codec also decodes the answer of the model.
`ask<Verdict>()` makes the model answer with a `Verdict` value instead of text.
Scry decodes the answer strictly. If a field is incorrect, Scry identifies the
field to the model and asks again. `send<Verdict>()` does the same from a main
loop. Refer to [examples/typed_answer.cpp](examples/typed_answer.cpp).

## Runtime

- **Non-blocking turns.** `send()` returns immediately. Each call to `update()`
  delivers the streamed text, runs the requested tools, and reports when the
  turn is complete. A time budget makes `update()` stop early to protect your
  frame time. For scripts and tests, `send_and_wait()` calls `update()` for you.
- **One worker thread.** Each `Harness` has one worker thread for network I/O.
  Scry calls your code only inside `update()`, on the calling thread, thus your
  tools do not need locks. Many conversations can share one `Harness`. Their
  turns run in first-in, first-out order. Each conversation can have one
  queued or active turn at a time.
- **Managed tool loop.** Scry runs each tool call and sends the result to the
  model until the model gives a final answer. Incorrect arguments, unknown
  tools, and refusals do not stop the turn. A hook can examine each tool call
  before it runs. Limits on tool rounds and tool calls set the maximum size of
  a turn.
- **Streamed output, retries, and cancellation.** Scry retries after network
  failures, rate limits, and server errors, with exponential backoff that obeys
  `Retry-After`. You can cancel or detach a turn at any time.
- **Atomic history.** A conversation records a turn only when the full turn
  succeeds. You can save a conversation to JSON and load it again.
- **Two provider dialects.** Scry supports the Anthropic Messages API and the
  OpenAI-compatible Chat Completions API, for example Ollama.
- **Errors as values.** Calls that can fail return `std::expected`. One
  `Config` holds all limits, and each limit has a safe default.
- **Scripted test provider.** The optional `scry::testing` library supplies a
  scripted HTTP server on loopback, to test your tools without a model.

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
| [examples/toolbox.cpp](examples/toolbox.cpp) | Each way to package a reflected tool in one program. `--tool-manifest` prints the generated schemas. |
| [examples/tool_policy.cpp](examples/tool_policy.cpp) | A tool refuses a call with a message, and the model tries again. |
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
