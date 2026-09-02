# API Reference {#api_reference}

This view describes the contract available to applications. The stable surface is C++23; the
optional `scry::reflection` component is an explicitly isolated, experimental C++26 capability.
Implementation details may change without notice and live in the @ref source_documentation
"Source Documentation" instead.

## Core concepts

| Concept | Responsibility |
|---|---|
| @ref scry::Config "Config" | Selects the provider and model, and sets operational and resource bounds. |
| @ref scry::Conversation "Conversation" | Owns transactionally committed, application-persistable history. |
| @ref scry::ToolRegistry "ToolRegistry" | Holds explicit-schema tools, snapshotted for each accepted turn. |
| @ref scry::Turn "Turn" | Move-only handle that identifies and cooperatively cancels one exchange. |
| @ref scry::Harness "Harness" | Owns the configured runtime, worker, tools, and callback pump. |

Supporting value types cover @ref scry::Json "JSON", @ref scry::Error "errors",
@ref scry::Completion "completed turns", @ref scry::TurnCallbacks "callbacks", and
@ref scry::UpdateStats "pump results". Fallible operations return @ref scry::Result "Result"
or @ref scry::Status "Status" rather than throwing Scry-originated semantic failures.

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
@ref scry::Harness::update "Harness::update()" on the thread that calls it.
@ref scry::Harness::send "Harness::send()" never waits for network I/O. The explicitly named
@ref scry::Harness::send_and_wait "Harness::send_and_wait()" convenience is the sole blocking
exception.

## Tool registration

The stable C++23 substrate accepts a @ref scry::ToolDefinition "ToolDefinition" and a move-only
@ref scry::ToolHandler "ToolHandler". The handler receives canonical JSON and returns a
@ref scry::Result "Result" of @ref scry::Json "Json". Registration is additive, duplicate names
are rejected, and accepted turns retain immutable snapshots.

The optional, experimental `scry::reflection` package component uses C++26 P2996 reflection to
generate schemas and strictly marshal typed arguments and results. It lowers into the same
registry rather than creating a second dispatch path. Core-only consumers remain ordinary C++23
programs and receive no reflection headers or compiler flags.

## Lifetime and error model

Failures before a turn is accepted are returned as @ref scry::Result "Result"; afterwards, every
outcome uses the single terminal channel @ref scry::TurnCallbacks::on_finished "on_finished".
When that optional callback is non-empty, it receives exactly one result by value: the completion
on success or the @ref scry::Error "Error" on failure, including cancellation as
@ref scry::ErrorCategory::cancelled "ErrorCategory::cancelled". Terminal processing still occurs
when the callback is empty.

Successful completion commits the full conversation exchange atomically; error and cancellation
commit nothing. Dropping @ref scry::Turn "Turn" detaches without cancellation or blocking.
@ref scry::Turn::cancel "Turn::cancel()" is an explicit cooperative request. Streamed
`std::string_view` and `const ToolCall&` observer arguments are borrowed only for the callback
invocation and must be copied if retained; `on_finished` receives its result by value.

## Where to go next

- Start from the umbrella header `scry/scry.hpp` for the stable core surface.
- Read the type and member pages linked above for exact preconditions, ownership, and failure
  behavior.
- Switch to the @ref source_documentation "Source Documentation" when debugging internals or
  preparing a contribution.
- Consult `docs/requirements.md` in the source repository whenever generated prose and a binding
  requirement appear to disagree.
