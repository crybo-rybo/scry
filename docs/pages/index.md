# Scry C++ API {#mainpage}

Scry is a C++26 LLM harness for applications that own their main loop. It puts
network I/O, streaming, retries, and the agentic tool loop behind a small API
that the host pumps. The host keeps control of its threads, its state, and its
lifecycle. Scry needs GCC 16 or newer, because its public API uses C++26
reflection (P2996).

This site is the generated reference for the exported API. The public headers
are the source of truth for this reference. For an introduction and a complete
program, see `README.md` in the source repository. For how the library is
built, what it guarantees, and its operation limits, see `docs/architecture.md`.

The public API has five core concepts:

- `scry::Config` selects the provider and defines the operation limits.
- `scry::Conversation` owns the committed history. Scry commits to this history
  only in transactions.
- `scry::ToolRegistry` holds the tools. Scry takes a snapshot of these tools for
  each accepted turn.
- `scry::Turn` is a move-only handle to one asynchronous exchange: `id()`,
  `finished()`, `cancel()`, and `disconnect()`.
- `scry::Harness` owns the configured runtime, the worker, the tools, and the
  callback pump.

## Minimal main-loop integration

This fragment uses render functions and frame functions that the host supplies:

```cpp
#include <scry/scry.hpp>

auto harness = scry::Harness::create(scry::Config{
    .base_url = "http://127.0.0.1:11434/v1",
    .model = "qwen3:8b",
    .dialect = scry::ProviderDialect::openai_compatible,
});
if (!harness) { render_error(harness.error()); return; }

auto conversation = scry::Conversation::create();
if (!conversation) { render_error(conversation.error()); return; }

auto turn = harness->send(*conversation, "Give me one useful observation.",
    scry::TurnCallbacks{
        .on_text_delta = [](std::string_view text) { render_streamed_text(text); },
        .on_finished = [](scry::Result<scry::Completion> outcome) {
          if (outcome) { render_final_answer(outcome->text); }
          else { render_error(outcome.error()); }
        },
    });
if (!turn) { render_error(turn.error()); return; }

while (application_is_running()) {
  harness->update();
  run_application_frame();
}
```

Each callback and each tool handler runs inside `scry::Harness::update()`, on
the thread that calls `update()`. `scry::Harness::send()` never waits for
network I/O.

`examples/main_loop.cpp` in the source repository is a complete program.
`docs/architecture.md` specifies the threading and lifetime rules, the tool
registration (reflected and explicit-schema), and the error and history model.
The optional `scry::testing` package component replaces the HTTP transfer with a
scripted transport. It replaces nothing else. `examples/testing_scripted.cpp` is
a complete test that uses this component.
