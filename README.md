# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++26 LLM harness for applications that own their main loop. Scry handles
HTTP, SSE streaming, tool dispatch, automatic resend of tool results, retries,
and transactional history. `send()` returns without waiting for network I/O;
callbacks and tool handlers run when your thread calls `update()`.

Declare tools with plain C++ aggregates and Scry generates their JSON schemas,
argument decoding, and result encoding using reflection. Explicit JSON schemas
and handlers are also supported. Scry is pre-1.0: API, ABI, and persistence-format
stability are not promised.

## Requirements

- **GCC 16 or newer**, with `-std=c++26 -freflection` and the P2996/P3394
  features checked by CMake. Clang and MSVC consumer builds are unsupported.
- **CMake 3.28** and **libcurl 7.84** or newer, with development headers.
  libcurl must provide thread-safe global initialization and asynchronous DNS.
- **Linux or macOS.** The CI matrix uses GCC 16 on Ubuntu 24.04 and macOS 15.

Glaze is a private header-only dependency. CMake uses an installed Glaze package
or fetches the pinned source. Tests additionally fetch Catch2. See
[Contributing](docs/contributing.md) for the development toolchain.

## Install

From the repository root, build and install the library:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-16 \
  -DSCRY_BUILD_TESTS=OFF -DSCRY_BUILD_EXAMPLES=OFF
cmake --build build/release
cmake --install build/release --prefix /your/prefix
```

Configure the consuming project with GCC 16 and
`-DCMAKE_PREFIX_PATH=/your/prefix`, then link the exported target:

```cmake
find_package(scry 0.3.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

Or use `FetchContent` in a CMake 3.28+ project configured with GCC 16. Scry's
tests and examples default to off when embedded; format targets are opt-in:

```cmake
include(FetchContent)
FetchContent_Declare(
  scry
  GIT_REPOSITORY https://github.com/crybo-rybo/scry.git
  GIT_TAG v0.3.0
)
FetchContent_MakeAvailable(scry)
target_link_libraries(app PRIVATE scry::scry)
```

## A complete program

```cpp
#include <chrono>
#include <iostream>
#include <scry/scry.hpp>
#include <string>
#include <thread>

// The schema, the strict argument decode, and the result encode are generated
// from these aggregates. The member annotation describes the parameter.
struct StatusArguments {
  [[= scry::reflection::description{
      "Include a human-readable state label in the result"}]] bool verbose{false};
};

struct StatusResult {
  bool running{};
  std::string state{};
};

int main() {
  // Assumes `ollama serve` is running and `ollama pull qwen3:8b` has completed.
  auto harness = scry::Harness::create({
      .base_url = "http://127.0.0.1:11434/v1",
      .model = "qwen3:8b",
      .dialect = scry::ProviderDialect::openai_compatible,
  });
  if (!harness) { std::cerr << harness.error().message << '\n'; return 1; }

  const auto registered = scry::reflection::add<StatusArguments>(
      harness->tools(),
      {.name = "get_application_status",
       .description = "Report whether the host application's main loop is running"},
      [](StatusArguments arguments) {
        return StatusResult{.running = true,
                            .state = arguments.verbose ? "main loop running" : ""};
      });
  if (!registered) { std::cerr << registered.error().message << '\n'; return 1; }

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

## How it works

- **A worker actor plus a pump on your thread.** One worker thread per `Harness`
  performs network I/O. Use the Harness and its handles from one host thread;
  `update()` delivers callbacks there. Turns on one Harness run in FIFO order.
- **A sans-I/O loop machine.** The agentic loop is a pure state machine that
  consumes events and emits commands and touches no network, file, or clock, so
  retries, cancellation, and multi-round tool use are tested deterministically.
- **Tools run inside `update()`.** Handlers are ordinary app code on the app's own
  thread, which is why they can touch host-owned game, GUI, or simulation state
  without a lock. A slow handler costs frame time; Scry never preempts your code.
- **Two dialects from `Config` alone.** Anthropic Messages, and a strict
  OpenAI-compatible Chat Completions subset. An empty API key is supported for
  unauthenticated servers that implement that subset.
- **Readable JSON and history.** `scry::JsonView` reads the Scry-owned `Json`
  boundary type and `scry::escape_json_string()` writes one, so no third-party
  parser is needed. `Conversation::messages()` exposes committed history as the
  public message model, and `to_json()`/`from_json()` persist it.
- **Cancel and disconnect.** `Turn::cancel()` stops the work and still reports the
  outcome; `Turn::disconnect()` clears callbacks while tools and history processing
  continue. Keep calling `update()` until the turn finishes.

## More

- [Architecture](docs/architecture.md) — how it is built, what it guarantees, and
  its operating limits.
- [Contributing](docs/contributing.md) — toolchain setup, presets, gates, and
  what a change needs before it lands.
- API reference: `./scripts/ci-docs.sh` writes the warning-clean Doxygen site to
  `build/docs/html/index.html`.
- [examples/main_loop.cpp](examples/main_loop.cpp) — the canonical example, with
  both registration paths and a rendered history.
- [extras/showcase](extras/showcase) — a standalone Dear ImGui chat panel and a
  grid world where the model drives an NPC through tools.

## License

Scry is released under the MIT License ([LICENSE](LICENSE)).
