# Scry C++ API {#mainpage}

Scry is a C++23 LLM harness for applications that own their main loop. It keeps network I/O,
streaming, retries, and the agentic tool loop behind a small pump-driven API while leaving the
host in control of its threads, state, and lifecycle.

The stable surface has five core concepts:

- `scry::Config` selects the provider and defines operational bounds.
- `scry::Conversation` owns transactionally committed history.
- `scry::ToolRegistry` holds explicit-schema tools snapshotted for each accepted turn.
- `scry::Turn` identifies and cancels one accepted asynchronous exchange.
- `scry::Harness` owns the configured runtime, worker, tools, and callback routes.

## Minimal main-loop integration

```cpp
#include <scry/scry.hpp>

auto harness = scry::Harness::create(scry::Config{
    .base_url = "http://127.0.0.1:11434",
    .model = "qwen3:1.7b",
    .dialect = scry::ProviderDialect::openai_compatible,
});

auto conversation = scry::Conversation::create();
auto turn = harness->send(
    *conversation, "Give me one useful observation.",
    {
        .on_text_delta =
            [](std::string_view text) { render_streamed_text(text); },
        .on_finished =
            [](scry::Result<scry::Completion> finished) {
              if (finished) {
                render_final_answer(finished->text);
              } else {
                render_error(finished.error().message);
              }
            },
    });
if (!turn) {
  render_error(turn.error().message);
}

while (application_is_running()) {
  harness->update();
  run_application_frame();
}
```

Every asynchronous callback and every tool handler runs inside
`scry::Harness::update()` on the thread that calls it. `scry::Harness::send()` never waits for
network I/O. The explicitly named `scry::Harness::send_and_wait()` convenience is the sole
blocking exception.

## Tool registration

The stable C++23 substrate accepts a `scry::ToolDefinition` and a move-only
`scry::ToolHandler`. The handler receives canonical JSON and returns a `scry::Result<scry::Json>`.
Registration is additive, duplicate names are rejected, and accepted turns retain immutable
snapshots.

The optional `scry::reflection` package component uses C++26 P2996 reflection to generate schemas
and strictly marshal typed arguments and results. It lowers into the same registry rather than
creating a second dispatch path. Core-only consumers remain ordinary C++23 programs and receive
no reflection headers or compiler flags.

## Lifetime and error model

Failures before a turn is accepted are returned as `scry::Result`. Callbacks are attached
infallibly and atomically as part of accepting `send()`, before an event can be published; there is
no late callback registration or replay. After acceptance, success, failure, and cancellation use
the single `on_finished(scry::Result<scry::Completion>)` channel. When non-empty, `on_finished`
runs exactly once unless Harness destruction begins first. Empty callbacks are legal, and terminal
processing still commits a successful exchange atomically while error and cancellation commit
nothing.

Dropping `scry::Turn` detaches without cancellation, blocking, or suppressing its send-time
callbacks. `scry::Turn::cancel()` is an explicit cooperative request and returns `false` after the
turn is terminal. Streamed `std::string_view` values and `const scry::ToolCall&` observers are
borrowed only for the callback invocation and must be copied if retained; `on_finished` receives
its result by value.

Callbacks may call `send()`, `cancel()`, and tool registration. A reentrant `update()` performs no
work, leaves queued events untouched, and reports `callbacks_delivered == 0` with
`budget_exhausted == true`.

For complete working code, see `examples/main_loop.cpp` in the source repository. Architectural
rationale and binding behavioral requirements remain in `DESIGN.md`, `ARCHITECTURE.md`, and
`REQUIREMENTS.md`.
