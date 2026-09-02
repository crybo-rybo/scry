# Scry C++ API {#mainpage}

Scry is a C++26 LLM harness for applications that own their main loop. It keeps
network I/O, streaming, retries, and the agentic tool loop behind a small
pump-driven API while leaving the host in control of its threads, state, and
lifecycle. It requires GCC 16 or newer, because its public API uses C++26
reflection (P2996).

This site is the generated reference for the exported API; the public headers are
its source of truth. For an orientation and a complete program, see `README.md`
in the source repository. For how the library is built, what it guarantees, and
which simplifications are deliberate, see `docs/architecture.md`.

The public surface has five core concepts:

- `scry::Config` selects the provider and defines operational bounds.
- `scry::Conversation` owns transactionally committed history.
- `scry::ToolRegistry` holds the tools snapshotted for each accepted turn.
- `scry::Turn` is a move-only handle to one asynchronous exchange: `id()`,
  `finished()`, `cancel()`, and `disconnect()`.
- `scry::Harness` owns the configured runtime, worker, tools, and callback pump.

## Minimal main-loop integration

```cpp
#include <scry/scry.hpp>

auto harness = scry::Harness::create(scry::Config{
    .base_url = "http://127.0.0.1:11434/v1",
    .model = "qwen3:8b",
    .dialect = scry::ProviderDialect::openai_compatible,
});

auto conversation = scry::Conversation::create();

auto turn = harness->send(*conversation, "Give me one useful observation.",
    scry::TurnCallbacks{
        .on_text_delta = [](std::string_view text) { render_streamed_text(text); },
        .on_finished = [](scry::Result<scry::Completion> outcome) {
          if (outcome) { render_final_answer(outcome->text); }
          else { render_error(outcome.error()); }
        },
    });

while (application_is_running()) {
  harness->update();
  run_application_frame();
}
```

Callbacks are supplied when the turn is created, so no event can arrive before
its handler exists. Every asynchronous callback, and every tool handler, runs
inside `scry::Harness::update()` on the thread that calls it.
`scry::Harness::send()` never waits for network I/O; the explicitly named
`scry::Harness::send_and_wait()` convenience is the sole blocking exception.

## Tool registration

`scry::reflection` is the primary registration path. It uses C++26 P2996
reflection to generate the input schema and to strictly marshal typed arguments
and results from a plain struct, with the `scry::reflection::description`
annotation as the one source of a tool or parameter description. It lowers into
the same registry as the explicit-schema path rather than creating a second
dispatch path, and it ships in the same `scry::scry` target.

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
receives exactly one result by value: the completion on success or the
`scry::Error` on failure — including cancellation, as
`scry::ErrorCategory::cancelled`. Terminal processing still occurs when the
callback is empty. Successful completion commits the full conversation exchange
atomically; error and cancellation commit nothing.

Dropping `scry::Turn` detaches without cancelling or blocking.
`scry::Turn::cancel()` is an explicit cooperative request that stops the work and
still reports the outcome, while `scry::Turn::disconnect()` keeps the work
running and clears every callback, so a host whose UI object is about to die can
sever delivery without cancelling. `scry::Harness::cancel(TurnId)` and
`scry::Harness::disconnect(TurnId)` are the same operations addressed by
identifier. `scry::Turn::finished()` reports whether the terminal outcome has
been delivered, so a poll loop can stop on the turn rather than on a mirrored
flag, and `scry::Conversation::messages()` exposes the committed history —
borrowed until the next committing `update()` — for a UI that renders it. The
streamed `std::string_view` and `const scry::ToolCall&` observer arguments are
borrowed only for the callback invocation and must be copied if retained;
`on_finished` receives its result by value.

For complete working code, see `examples/main_loop.cpp` in the source repository.
