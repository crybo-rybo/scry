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

Callbacks are supplied when the turn is created, so no event can arrive before
its callback set is attached. Use each Harness and its handles from one host
thread. Every asynchronous callback and tool handler runs inside
`scry::Harness::update()` on the thread that calls it.
`scry::Harness::send()` never waits for network I/O; the explicitly named
`scry::Harness::send_and_wait()` pumps until completion. Harness destruction
also waits for the worker to stop.

## Tool registration

`scry::reflection` is the primary registration path. It uses C++26 P2996
reflection to generate the input schema and to strictly marshal typed arguments
and results from a plain struct. Member annotations using
`scry::reflection::description` supply parameter descriptions; `ToolMetadata`
supplies the tool name and description. Both registration paths use the same
registry and ship in the `scry::scry` target.

The explicit-schema path is the escape hatch for tools whose schema exists only
at runtime. It accepts a `scry::ToolDefinition` and a move-only
`scry::ToolHandler` that receives canonical JSON and returns a
`scry::Result<scry::Json>`. Such a handler reads its arguments with
`scry::JsonView` — value kind, `find()`, `at()`, and ordered `key_at()` — and
builds small results with `scry::escape_json_string()`; neither needs a
third-party parser. Registration is additive, duplicate names are rejected, and
accepted turns retain immutable snapshots. `scry::ToolRegistry::contains()` and
`names()` report what is registered.

## Lifetime and error model

Failures before a turn is accepted are returned as `scry::Result`; afterwards,
every outcome uses the single terminal channel
`scry::TurnCallbacks::on_finished`. When that optional callback is non-empty, it
receives one result by value while the host keeps pumping, unless disconnected
or discarded by Harness destruction: the completion on success or the
`scry::Error` on failure — including cancellation, as
`scry::ErrorCategory::cancelled`. Terminal processing still occurs when the
callback is empty. Successful completion commits the full conversation exchange
atomically; error and cancellation commit nothing.

Dropping `scry::Turn` leaves work and callbacks active without cancelling or
blocking.
`scry::Turn::cancel()` is an explicit cooperative request that stops the work and
still reports the outcome, while `scry::Turn::disconnect()` keeps the work
running and clears every callback, so a host whose UI object is about to die can
sever delivery without cancelling. Tools still run, and the host must keep
pumping for the turn to finish. `scry::Harness::cancel(TurnId)` and
`scry::Harness::disconnect(TurnId)` are the same operations addressed by
identifier. `scry::Turn::finished()` reports whether the terminal outcome has
been delivered, or processed when no terminal callback is attached, so a poll
loop can stop on the turn itself. `scry::Conversation::messages()` exposes
committed history, borrowed until a committing `update()` or until the handle is
moved or destroyed. The
streamed `std::string_view` and `const scry::ToolCall&` observer arguments are
borrowed only for the callback invocation and must be copied if retained;
`on_finished` receives its result by value.

For complete working code, see `examples/main_loop.cpp` in the source repository.
