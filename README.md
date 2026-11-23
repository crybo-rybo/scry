# Scry

Scry is a C++26 library for putting an LLM inside your application. You send a
message; Scry streams the reply, runs the C++ functions the model calls as
tools, and keeps the conversation history.

A tool is an ordinary annotated function. Static reflection generates its JSON
schema and argument decoding, so there is no JSON to write. Network I/O happens
on a background thread, and your tools and callbacks run on yours.

Scry works with Anthropic and with any OpenAI-compatible server, such as Ollama.

## Quickstart

<!-- examples/quickstart.cpp -->
```cpp
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <utility>

// Any annotated function can be a tool. Reflection generates its JSON schema,
// argument decoder, and result encoder from the signature.
[[= scrydantic::tool{"Get the current weather in a city"}]] std::string
    weather(std::string city) {
  return city + ": 18C and sunny";
}

int main() {
  scry::ToolRegistry tools;
  if (auto added = tools.add<^^weather>(); !added) {
    std::cerr << added.error().message << '\n';
    return 1;
  }

  // A local Ollama server: `ollama pull qwen2.5:7b`.
  auto harness =
      scry::Harness::create({.base_url = "http://127.0.0.1:11434/v1",
                             .model = "qwen2.5:7b",
                             .dialect = scry::ProviderDialect::openai_compatible},
                            std::move(tools));
  auto conversation = scry::Conversation::create();
  if (!harness || !conversation) {
    std::cerr << (harness ? conversation.error() : harness.error()).message << '\n';
    return 1;
  }

  auto reply =
      harness->send_and_wait(*conversation, "Do I need an umbrella in Oslo today?");
  std::cout << (reply ? reply->text : reply.error().message) << '\n';
}
```

```
The weather in Oslo today is sunny with a temperature of 18C. You won't need an umbrella.
```

## Install

You need GCC 16.2 or newer, CMake 3.30 or newer, and libcurl 7.84 or newer, on
Linux or macOS. Clang and MSVC do not support C++26 reflection yet.

```cmake
include(FetchContent)
FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.8.0
)
FetchContent_MakeAvailable(scry)
target_link_libraries(app PRIVATE scry::scry)
```

The `scry::scry` target brings the compiler flags with it, along with
[Scrydantic](https://github.com/crybo-rybo/scrydantic), the library that does the
reflection.

To use an installed copy instead, build and install Scry:

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-16 -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
cmake --build build/release
cmake --install build/release --prefix /your/prefix
```

Then configure your project with `-DCMAKE_PREFIX_PATH=/your/prefix`:

```cmake
find_package(scry 0.8.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

Scry is pre-1.0: a minor release can change the API.

## Learn more

The [guide](docs/guide.md) covers each task in a few lines. The public headers in
[include/scry/](include/scry/) are the API reference. Each example is one short
file:

| Example | Shows |
|---|---|
| [quickstart.cpp](examples/quickstart.cpp) | One tool and one question |
| [main_loop.cpp](examples/main_loop.cpp) | Streaming a reply from a frame loop |
| [tools.cpp](examples/tools.cpp) | Tools that share state, constrain their arguments, and refuse calls |
| [typed_answer.cpp](examples/typed_answer.cpp) | A reply that is a C++ struct |
| [testing_scripted.cpp](examples/testing_scripted.cpp) | Testing your application without a model |

## Build and test Scry

```sh
./scripts/test.sh                # PRESET=asan or PRESET=tsan for the sanitizers
./scripts/format.sh --fix
./scripts/lint.sh
```

Set `CXX` if your GCC 16 is not named `g++-16`.

## License

[MIT](LICENSE)
