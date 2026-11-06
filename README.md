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

The model reads the tool schema, calls `subsystem_state`, and answers. With
a local `qwen2.5:7b`, the program printed:

```
The renderer is currently running.
```

This program is [examples/quickstart.cpp](examples/quickstart.cpp). A test
checks that the block above is the file, and CI compiles it.

## One declaration makes the full tool

```cpp
struct WaterArguments {
  [[= scrydantic::description{"Bed number, from 1 to 3"}]] std::int32_t bed;
  [[= scrydantic::description{"Liters to pour"}]] double liters{1.0};
};

[[= scrydantic::tool{"Water one bed"}]] scrydantic::Status
    water(WaterArguments arguments);
```

Scry makes this schema from the declaration, and the model gets it under the
tool name `water`:

```json
{
  "additionalProperties": false,
  "properties": {
    "bed": {
      "description": "Bed number, from 1 to 3",
      "maximum": 2147483647,
      "minimum": -2147483648,
      "type": "integer"
    },
    "liters": { "description": "Liters to pour", "type": "number" }
  },
  "required": ["bed"],
  "type": "object"
}
```

- **The types set the schema.** `std::int32_t` gives an integer with the range
  of the type. `liters` has a default value, thus it is not required.
- **The same types decode the arguments.** Scry rejects a missing required
  member, an incorrect type, a value out of range, and an unknown member. The
  model gets an error message that identifies the field, and the turn
  continues. Your function gets only valid arguments.
- **Errors occur at compile time.** If a declaration uses a type that Scry
  cannot encode, the compiler stops. The diagnostic gives the member and the
  reason.

Scry supports `bool`, integers, floating-point types, `std::string`, enums (as
strings), nested structs, `std::vector`, `std::array`, `std::optional`, and
tagged `std::variant`. Run [examples/toolbox.cpp](examples/toolbox.cpp) with
`--tool-manifest` to print the schema of every tool in a program. This needs
no model and no network.

The [guide](docs/guide.md) covers the rest: every way to package tools,
typed answers, the main loop, providers, errors, persistence, and testing
without a model. [docs/architecture.md](docs/architecture.md) states the
guarantees and the limits. Read it before you rely on a specific behavior.

## Requirements

- **GCC 16 or newer.** The public headers use C++26 reflection and need
  `-std=c++26 -freflection`. Scry does not support Clang or MSVC.
- **CMake 3.30 or newer** and Ninja. 3.30 is the first CMake version that
  knows the C++26 mode of GCC.
- **libcurl 7.84 or newer**, with development headers.
- **Scrydantic 0.1.x.** Scry uses it for JSON, schemas, and reflected tools.
  CMake uses an installed package when available, otherwise fetches a pinned
  revision.
- **Linux or macOS.** CI uses GCC 16 on Ubuntu 24.04.

## Install

Build and install from the repository root:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
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

| Option | Default | Purpose |
|---|---|---|
| `SCRY_BUILD_TESTS` | On at top level | Build the tests |
| `SCRY_BUILD_EXAMPLES` | On at top level | Compile the programs under `examples/` |
| `SCRY_BUILD_TESTING_SUPPORT` | On | Build and install `scry::testing`, the scripted server for tests without a model |
| `SCRY_WARNINGS_AS_ERRORS` | On at top level | Treat project warnings as errors |
| `SCRY_SANITIZER` | `none` | `none`, `address-undefined`, or `thread`. Also instruments a fetched Scrydantic |
| `SCRY_FETCH_SCRYDANTIC` | On | Fetch the pinned Scrydantic when no installed package or parent target exists |

To require an installed Scrydantic and prevent a download, set
`-DSCRY_FETCH_SCRYDANTIC=OFF` and set `CMAKE_PREFIX_PATH` or `scrydantic_DIR`.
For local Scrydantic development, set `FETCHCONTENT_SOURCE_DIR_SCRYDANTIC` to
your checkout. A parent project can also supply the `scrydantic::scrydantic`
and `scrydantic::json` targets before it adds Scry.

## Build and test

```sh
./scripts/test.sh                  # configure, build, parallel ctest (dev preset)
./scripts/test.sh -R 'runtime\.'   # the script sends extra arguments to ctest
PRESET=asan ./scripts/test.sh      # or PRESET=tsan
./scripts/format.sh --fix          # --check (the default) only verifies
./scripts/lint.sh                  # clang-tidy on the kernel, plus the repository rules
./scripts/test-e2e.sh              # live model; see the header of the script
```

CI runs these scripts: the format check and the lint in one job, and
`test.sh` with the `dev`, `asan`, and `tsan` presets in another. The presets
select `g++-16` and build into `build/<preset>` with Ninja. If your compiler
has a different name, set `CXX` for the scripts. The formatter is clang-format
and the linter is clang-tidy, version 22 or newer; older versions format
reflection annotations differently. Set `CLANG_FORMAT` and `CLANG_TIDY` to
use a specific binary. `.clangd` points editors at `build/dev`.

The tests download Catch2. `tsan` runs only on Linux, because GCC supplies no
thread-sanitizer runtime on Apple Silicon.

## More

| Resource | Content |
|---|---|
| [docs/guide.md](docs/guide.md) | How to use Scry, by task |
| [docs/architecture.md](docs/architecture.md) | The design, the guarantees, and the operation limits |
| [include/scry/](include/scry/) | The API reference, in the public headers |
| [examples/main_loop.cpp](examples/main_loop.cpp) | A reflected tool, a dynamic tool, an admission hook, and the history |
| [examples/toolbox.cpp](examples/toolbox.cpp) | Each way to package a reflected tool in one program |
| [examples/tool_policy.cpp](examples/tool_policy.cpp) | A tool refuses a call with a message, and the model tries again |
| [examples/typed_values.cpp](examples/typed_values.cpp) | The reflected codec alone: an answer type, its schema, a strict decode, and the error text |
| [examples/typed_answer.cpp](examples/typed_answer.cpp) | A turn that ends with a C++ value |
| [examples/seeded_trials.cpp](examples/seeded_trials.cpp) | The same prompt, many times, with a fixed sampling seed |
| [examples/testing_scripted.cpp](examples/testing_scripted.cpp) | A downstream test with a scripted server on loopback and no model |
| [extras/showcase](extras/showcase) | A Dear ImGui chat panel, and a grid world where the model controls an NPC with tools |
| [docs/releases](docs/releases) | The notes of each release |

## License

Scry is released under the MIT License. Refer to [LICENSE](LICENSE).
