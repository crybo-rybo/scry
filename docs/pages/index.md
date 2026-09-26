# Scry C++ API {#mainpage}

Scry is a C++26 LLM harness for applications that own their main loop. It keeps
network I/O, streaming, retries, and the agentic tool loop behind a small
pump-driven API while leaving the host in control of its threads, state, and
lifecycle. It requires GCC 16 or newer, because its public API uses C++26
reflection (P2996).

This site is the generated reference for the exported API; the public headers are
its source of truth. For an orientation and a complete program, see `README.md`
in the source repository. For how the library is built, what it guarantees, and
its operating limits, see `docs/architecture.md`.

The public surface has five core concepts:

- `scry::Config` selects the provider and defines operational bounds.
- `scry::Conversation` owns transactionally committed history.
- `scry::ToolRegistry` holds the tools snapshotted for each accepted turn.
- `scry::Turn` is a move-only handle to one asynchronous exchange: `id()`,
  `finished()`, `cancel()`, and `disconnect()`.
- `scry::Harness` owns the configured runtime, worker, tools, and callback pump.

## Minimal main-loop integration

This fragment uses host-provided rendering and frame functions:

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

Every callback and tool handler runs inside `scry::Harness::update()` on the
thread that calls it; `scry::Harness::send()` never waits for network I/O.

`examples/main_loop.cpp` in the source repository is a complete program. The
threading and lifetime rules, tool registration (reflected and explicit-schema),
and the error and history model are specified in `docs/architecture.md`. The
optional `scry::testing` package component substitutes a scripted transport for
the HTTP transfer and nothing else; `examples/testing_scripted.cpp` is a complete
test in that shape.
