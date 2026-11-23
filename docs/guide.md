# Scry guide

Each section is one task and the least code that does it. The headers under
[include/scry/](../include/scry/) document every type and option.

- [How it fits together](#how-it-fits-together)
- [Choose a model](#choose-a-model)
- [Declare tools](#declare-tools)
- [Refuse or limit tool calls](#refuse-or-limit-tool-calls)
- [Get a typed answer](#get-a-typed-answer)
- [Run in a main loop](#run-in-a-main-loop)
- [Handle errors](#handle-errors)
- [Save a conversation](#save-a-conversation)
- [Test without a model](#test-without-a-model)

## How it fits together

| Object | What it is |
|---|---|
| `ToolRegistry` | The tools the model may call |
| `Harness` | A connection to one model. Owns the registry and a network thread |
| `Conversation` | A system prompt and the message history. Many can share a Harness |
| `Turn` | One message and everything up to its final answer, tool calls included |

A turn either succeeds and commits its messages to the conversation, or fails and
commits nothing.

Use a Harness and its conversations from one thread, and destroy the Harness
before anything its tools and callbacks refer to.

`scry::` is the conversation with the model. `scrydantic::` is C++ types as JSON:
the annotations, and what a tool returns. `<scry/scry.hpp>` includes both. The
[Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md)
lists every supported type and annotation.

## Choose a model

```cpp
// Anthropic
scry::Config config{.base_url = "https://api.anthropic.com",
                    .api_key = key,
                    .model = "claude-sonnet-5-5"};

// OpenAI-compatible: Ollama, vLLM, llama.cpp, OpenAI, ...
scry::Config config{.base_url = "http://127.0.0.1:11434/v1",
                    .model = "qwen2.5:7b",
                    .dialect = scry::ProviderDialect::openai_compatible};
```

`Config` also holds sampling, retries, timeouts, size limits, a proxy, a CA
bundle, and extra headers, all documented in
[config.hpp](../include/scry/config.hpp). `sampling.max_tokens` defaults to 1024.
A slow local model can need a longer `timeouts.idle`.

`Harness::validate(config)` checks a configuration without starting anything.

## Declare tools

Annotate a function with `scrydantic::tool` and register it:

```cpp
[[= scrydantic::tool{"Get the current weather in a city"}]]
std::string weather(std::string city);

tools.add<^^weather>();          // one function
tools.add<^^weather_tools>();    // every annotated function in a namespace
tools.add(Greenhouse{});         // every annotated member function of an object
tools.add(shared_greenhouse);    // the same, through a std::shared_ptr you also hold
```

Each `add()` returns a `scry::Status`, and registers all of its tools or none.
Tool names must be unique.

Take one struct instead of plain parameters to describe the arguments, constrain
them, or give them defaults:

```cpp
struct WaterArguments {
  [[= scrydantic::description{"Bed number"}, = scrydantic::range{.min = 1, .max = 3}]]
  std::int32_t bed;
  double liters{1.0}; // optional, because it has a default
};

[[= scrydantic::tool{"Water one bed"}]]
scrydantic::Status water(WaterArguments arguments);
```

Your function only runs with valid arguments. For anything else the model gets
the reason, such as `$.bed must be at most 3`, and can call again.

When the name or description is only known at run time, register a callable:

```cpp
tools.add<WaterArguments>({.name = "water", .description = "Water one bed"},
                          [&](WaterArguments arguments) { return pour(arguments); });
```

`add_dynamic()` takes a hand-written JSON schema and a JSON handler, for tools
that have no C++ type at all. See
[tool_registry.hpp](../include/scry/tool_registry.hpp).

`tools.to_json()` returns every generated schema.

## Refuse or limit tool calls

A tool refuses a call by returning an error. The model reads the text, and the
turn goes on:

```cpp
return std::unexpected(scrydantic::tool_error("only 5 liters are left"));
```

To decide before a tool runs, use `on_tool_request`:

```cpp
auto turn = harness.send(conversation, text, {
    .on_tool_request = [&](const scry::ToolRequest& request)
        -> std::optional<scry::ToolRejection> {
      if (request.context.tool_name == "water" && !user_confirmed)
        return scry::ToolRejection{.model_message = "the user has not confirmed"};
      return std::nullopt; // run it
    },
});
```

`Config::max_tool_calls_per_turn` limits the calls in one turn, and
`Config::max_tool_rounds` (8 by default) limits the round trips. Scry cannot undo
what a tool already did when a turn fails or is cancelled.

## Get a typed answer

```cpp
struct Verdict {
  [[= scrydantic::description{"Is the claim true?"}]] bool supported{};
  [[= scrydantic::description{"One sentence explaining why"}]] std::string reason{};
};

auto answer = harness.ask<Verdict>(conversation, "Is the moon made of cheese?");
if (answer)
  std::cout << answer->value.reason << '\n';
```

The model can still call your tools first. A reply of the wrong shape goes back to
it with the reason. `send<Verdict>()` is the main-loop form: its `on_finished`
receives the same result.

## Run in a main loop

`send_and_wait()` and `ask()` block. In an application with its own loop, use
`send()`, which returns at once, and call `update()` every frame:

```cpp
auto turn = harness.send(conversation, "How is the player doing?", {
    .on_text_delta = [](std::string_view text) { std::cout << text; },
    .on_finished = [](scry::Result<scry::Completion> done) { /* once */ },
});

// each frame
harness.update({.time_budget = std::chrono::milliseconds{2}});
```

Tools and callbacks run inside `update()`, on the thread that calls it, so they
need no locks. The time budget cannot interrupt a tool that is already running.

| To | Do |
|---|---|
| Know when a turn is over | Use `on_finished`, or poll `turn->finished()` |
| Stop a turn | `turn->cancel()`. `on_finished` gets a `cancelled` error |
| Keep it running without callbacks | `turn->disconnect()` |
| Stop everything | Destroy the Harness |

Dropping the `Turn` handle does nothing to the turn. A conversation runs one turn
at a time, and a second `send()` on it fails with `busy`.

## Handle errors

Everything that can fail returns `scry::Result<T>`, a
`std::expected<T, scry::Error>`. A turn that fails after `send()` accepted it
reports through `on_finished`.

```cpp
if (!reply) {
  std::cerr << reply.error().message << '\n';
  if (reply.error().category == scry::ErrorCategory::rate_limit) { /* ... */ }
}
```

Scry already retries network failures, rate limits, and server errors, until the
first output arrives. [error.hpp](../include/scry/error.hpp) lists the categories.

A successful reply can still be cut short by the token limit. Check for
`finish_reason == scry::FinishReason::length`.

## Save a conversation

```cpp
auto saved = conversation.to_json();                           // saved->text
auto restored = scry::Conversation::from_json({.text = text});
```

The document holds the system prompt and the committed messages.
`conversation.messages()` reads them directly.

## Test without a model

`scry::testing` is a scripted HTTP server on 127.0.0.1. Point a Harness at it, and
your test runs all of Scry with canned replies:

```cmake
find_package(scry CONFIG REQUIRED COMPONENTS testing) # not needed with FetchContent
target_link_libraries(app_tests PRIVATE scry::scry scry::testing)
```

```cpp
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>

auto server = scry::testing::ScriptedServer::create();
server->enqueue(
    {.body_chunks = {scry::testing::anthropic_text_stream("No umbrella needed.")}});

auto harness = scry::Harness::create(
    {.base_url = server->url(), .api_key = "test-key", .model = "test-model"});
```

`server->requests()` returns what Scry sent.
[testing_scripted.cpp](../examples/testing_scripted.cpp) is a whole test with a
tool call.
