# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications that own their main loop. Scry hides the
complete agentic loop — HTTP, SSE streaming, tool dispatch, automatic resend of
tool results, retries, transactional history — behind a small poll-friendly API.
`send()` never waits on the network, and every callback and every tool handler
runs inside the `update()` you already call once a frame, on your own thread. No
event loop is imposed and application code needs no locks.

It is built for the apps that live in C++ — games, GUI tools, simulators, CAD,
trading systems — where you cannot block a frame, cannot shell out to Python, and
want tool use rather than just chat. Tools are declared with C++26 reflection: you
write a plain struct and Scry derives the JSON schema, the strict argument
decode, and the result encode from it at compile time. The name is the design —
reflection (the mirror) plus consulting an oracle (the LLM). Scry is pre-1.0: no
API or ABI stability is promised yet.

## Requirements

- **GCC 16 or newer**, with `-std=c++26 -freflection`. C++26 reflection (P2996) is
  part of the core public API and GCC 16 is currently the only compiler that
  implements it, so Clang and MSVC are unsupported until they ship it.
- **CMake 3.28** and **libcurl 7.84** or newer, with development headers.
- **Linux and macOS.** CI covers GCC 16 on Ubuntu 24.04 (from
  `ppa:ubuntu-toolchain-r/test`) and on macOS 15 (from Homebrew).

The implementation under `src/` stays portable C++23, so supporting a second
reflection compiler will be a build-matrix change rather than a port.

## Install

As an installed package:

```sh
cmake -S scry -B scry/build -DCMAKE_BUILD_TYPE=Release
cmake --build scry/build
cmake --install scry/build --prefix /your/prefix
```

```cmake
find_package(scry 0.3.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE scry::scry)
```

Or with `FetchContent` — Scry's tests, examples, and format targets stay off
automatically when it is not the top-level project:

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
// from these two aggregates. The annotation is the only source of a description.
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
    harness->update(); // every callback and tool handler runs here, this thread
    std::this_thread::sleep_for(std::chrono::milliseconds{1}); // your frame here
  }
  return 0;
}
```

## How it works

- **A worker actor plus a pump on your thread.** One worker thread per `Harness`
  does all blocking I/O; `update()` delivers everything back on the thread that
  calls it, so your code stays single-threaded by construction.
- **A sans-I/O loop machine.** The agentic loop is a pure state machine that
  consumes events and emits commands and touches no network, file, or clock, so
  retries, cancellation, and multi-round tool use are tested deterministically.
- **Tools run inside `update()`.** Handlers are ordinary app code on the app's own
  thread, which is why they can touch host-owned game, GUI, or simulation state
  without a lock. A slow handler costs frame time; Scry never preempts your code.
- **Two dialects from `Config` alone.** Anthropic Messages, and a strict
  OpenAI-compatible Chat Completions subset that also drives Ollama, vLLM, and
  llama.cpp server with no API key.
- **Readable JSON and history.** `scry::JsonView` reads the Scry-owned `Json`
  boundary type and `scry::escape_json_string()` writes one, so no third-party
  parser is needed. `Conversation::messages()` exposes committed history as the
  public message model, and `to_json()`/`from_json()` persist it.
- **Cancel and disconnect.** `Turn::cancel()` stops the work and still reports the
  outcome; `Turn::disconnect()` keeps the work running and stops the reporting, so
  a UI object that dies mid-turn can sever its callbacks without cancelling.

## More

- [Architecture](docs/architecture.md) — how it is built, what it guarantees, and
  which simplifications are deliberate.
- [Contributing](docs/contributing.md) — toolchain setup, presets, gates, and
  what a change needs before it lands.
- API reference: `./scripts/ci-docs.sh` writes the warning-clean Doxygen site to
  `build/docs/html/index.html`.
- [examples/main_loop.cpp](examples/main_loop.cpp) — the canonical example, with
  both registration paths and a rendered history.
- [extras/showcase](extras/showcase) — a standalone Dear ImGui chat panel and a
  grid world where the model drives an NPC through tools.
- [Releases](docs/releases/) — release notes, newest first.

## License

Scry is released under the MIT License ([LICENSE](LICENSE)).
