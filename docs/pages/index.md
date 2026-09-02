# Scry C++ API {#mainpage}

Scry is a C++23 LLM harness for applications that own their main loop. It keeps network I/O,
streaming, retries, and the agentic tool loop behind a small pump-driven API while leaving the
host in control of its threads, state, and lifecycle.

The stable surface has five core concepts:

- `scry::Config` selects the provider and defines operational bounds.
- `scry::Conversation` owns transactionally committed history.
- `scry::ToolRegistry` holds explicit-schema tools snapshotted for each accepted turn.
- `scry::Turn` is a move-only handle to one asynchronous exchange: `id()` and `cancel()`.
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

Callbacks are supplied when the turn is created, so no event can arrive before its handler
exists. Every asynchronous callback, and every tool handler, runs inside
`scry::Harness::update()` on the thread that calls it. `scry::Harness::send()` never waits for
network I/O. The explicitly named `scry::Harness::send_and_wait()` convenience is the sole
blocking exception.

## Tool registration

The stable C++23 substrate accepts a `scry::ToolDefinition` and a move-only
`scry::ToolHandler`. The handler receives canonical JSON and returns a `scry::Result<scry::Json>`.
It reads those arguments with `scry::JsonView` — value kind, `find()`, `at()`, and ordered
`key_at()` — and builds small results with `scry::escape_json_string()`; neither needs a
third-party parser. Registration is additive, duplicate names are rejected, and accepted turns
retain immutable snapshots. `scry::ToolRegistry::contains()` and `names()` report what is
registered.

The optional, experimental `scry::reflection` package component uses C++26 P2996 reflection to
generate schemas and strictly marshal typed arguments and results. It lowers into the same
registry rather than creating a second dispatch path. Core-only consumers remain ordinary C++23
programs and receive no reflection headers or compiler flags.

## Lifetime and error model

Failures before a turn is accepted are returned as `scry::Result`; afterwards, every outcome uses
the single terminal channel `scry::TurnCallbacks::on_finished`. When that optional callback is
non-empty, it receives exactly one result by value: the completion on success or the `scry::Error`
on failure — including cancellation, as `scry::ErrorCategory::cancelled`. Terminal processing
still occurs when the callback is empty. Successful completion commits the full conversation
exchange atomically; error and cancellation commit nothing.

Dropping `scry::Turn` detaches without cancellation or blocking. `scry::Turn::cancel()` is an
explicit cooperative request, and `scry::Harness::cancel(TurnId)` is the same request addressed by
identifier. `scry::Turn::finished()` reports whether the terminal outcome has been delivered, so a
poll loop can stop on the turn rather than on a mirrored flag, and
`scry::Conversation::messages()` exposes the committed history — borrowed until the next
committing `update()` — for a UI that renders it. The streamed `std::string_view` and `const scry::ToolCall&` observer
arguments are borrowed only for the callback invocation and must be copied if retained;
`on_finished` receives its result by value.

For complete working code, see `examples/main_loop.cpp` in the source repository. Architectural
rationale and binding behavioral requirements remain in `docs/design/`,
`docs/architecture/`, and `docs/requirements.md`.
