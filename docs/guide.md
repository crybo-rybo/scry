# Scry guide

How to use Scry, by task. Each section gives the minimal code, what it does,
and where the full example is. [architecture.md](architecture.md) states the
guarantees behind each behavior and the limits. The public headers under
[include/scry/](../include/scry/) are the API reference.

Every example assumes an OpenAI-compatible server on `127.0.0.1:11434`, such
as `ollama serve` with a model that supports tools. The programs name
`qwen3:8b`. To use another model, change `Config::model`. The outputs below
were recorded with a local `qwen2.5:7b`, and another model answers with other
words.

- [The pieces](#the-pieces)
- [Declare tools](#declare-tools)
- [Refuse a call, and let the model try again](#refuse-a-call-and-let-the-model-try-again)
- [Control which calls run](#control-which-calls-run)
- [Register a tool that has no C++ declaration](#register-a-tool-that-has-no-c-declaration)
- [Get a typed answer](#get-a-typed-answer)
- [Drive a turn from a main loop](#drive-a-turn-from-a-main-loop)
- [Configure the provider](#configure-the-provider)
- [Handle errors](#handle-errors)
- [Save and restore a conversation](#save-and-restore-a-conversation)
- [Test without a model](#test-without-a-model)

## The pieces

A program that uses Scry has four objects:

| Object | What it is |
| --- | --- |
| `scry::ToolRegistry` | The tools that the model can call. You build it, and the Harness takes it. |
| `scry::Harness` | One configured connection to a model: the provider settings, the registry, and a worker thread for the network. |
| `scry::Conversation` | A system prompt and the committed message history. Many conversations can share one Harness. |
| `scry::Turn` | The handle to one exchange that `send()` accepted: one user message, the tool rounds that the model asks for, and the final answer. |

A turn goes through these steps:

1. `harness.send(conversation, text, callbacks)` checks the request and
   returns a `Turn` at once.
2. The worker sends the request and reads the stream.
3. Each time you call `harness.update()`, Scry delivers what is ready on your
   thread: text deltas, and the tool calls of the model, which run your
   functions.
4. Scry sends the tool results back. Steps 2 to 4 repeat until the model
   answers without a tool call.
5. Inside `update()`, Scry commits the whole exchange to the conversation and
   then calls `on_finished`.

`send_and_wait()` and `ask<T>()` do steps 1 to 5 in one blocking call.

### Two libraries, two namespaces

Scry does not make schemas or parse JSON. It uses
[Scrydantic](https://github.com/crybo-rybo/scrydantic) for that, and
`<scry/scry.hpp>` includes it. The split is by namespace:

- **`scrydantic::`** is everything about a C++ type as JSON: the annotations
  on your structs and functions, `Json` and `JsonView`, `encode()`,
  `decode<T>()`, and `schema_v<T>`. The
  [Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md)
  is the reference for the supported types, the annotations, the constraints,
  the validators, and the text of a decode error.
- **`scry::`** is everything about a conversation with a model: the registry,
  the Harness, turns, callbacks, configuration, and history. This guide is
  about that.

Each library has its own `Error`, `Result<T>`, `Status`, and `tool_error()`.
Which one a function of yours returns depends on who calls it:

| You write | It returns | It refuses with |
| --- | --- | --- |
| A reflected tool: an annotated function or member function, or an `add<Args>()` handler | A value, `void`, `scrydantic::Result<T>`, or `scrydantic::Status` | `scrydantic::tool_error()` |
| A validator for `scrydantic::check` | `scrydantic::Status` | `scrydantic::tool_error()` |
| A dynamic tool, for `add_dynamic()` | `scry::Result<scrydantic::Json>` | `scry::tool_error()` |
| A hand-written `ResponseFormat::validate` | `scry::Status` | `scry::tool_error()` |

The rule: code that Scrydantic calls from a declaration returns Scrydantic's
types, and code that you give to Scry as a JSON callable returns Scry's. Both
`tool_error()` functions take the same two texts, one for the model and one
for the host. A `scrydantic::Error` has no category. Scry gives it one when it
reports it to you: `tool` for a tool call, and `invalid_argument` for a typed
answer. Everything that Scry returns to you is a `scry::Result` or a
`scry::Status`.

To cross from one to the other yourself, pass the `scrydantic::Error` to
`scry::tool_error()`. It keeps the texts and the issues and sets the category
`tool`. [A dynamic tool](#register-a-tool-that-has-no-c-declaration) uses it
to forward a decode failure.

## Declare tools

A tool is a C++ function that the model can call. Annotate it with
`scrydantic::tool` and register it. Scrydantic makes its schema, its argument
decoder, and its result encoder from the declaration. Scry keeps it in a
registry, sends the schema to the model, and calls the function when the model
asks.

Each registration form is optional. Use the form that agrees with your code,
or use many forms in one registry. All forms, except `add_dynamic()`, get the
schema from reflection.

| Form | Registration | Use it when |
| --- | --- | --- |
| Free function | `tools.add<^^to_fahrenheit>()` | One tool that has no state. |
| Namespace | `tools.add<^^almanac>()` | A group of tools that have no state. Each annotated function declared in the namespace before the call becomes a tool. |
| Toolbox object | `tools.add(Greenhouse{})` | Tools that share state. Each annotated member function becomes a tool bound to the object. The registry owns the object. |
| Shared toolbox | `tools.add(std::make_shared<Greenhouse>())` | Tools that share state, and your application also reads that state. |
| Lambda | `tools.add<WaterArguments>({.name = "water", .description = "Water one bed"}, handler)` | The name or description is known only at runtime, or the handler captures local data. Reflection still makes the schema from the struct. |
| Dynamic | `tools.add_dynamic(definition, handler)` | The tool exists only at runtime, for example a tool from a script. You write the JSON schema and read the JSON arguments. |

The first four forms in one program:

```cpp
// A free function. The name annotation changes the tool name for the model.
[[= scrydantic::tool{"Convert Celsius to Fahrenheit"},
  = scrydantic::name{"celsius_to_fahrenheit"}]]
double to_fahrenheit(double celsius);

// A namespace. Each annotated function is a tool.
namespace almanac {
[[= scrydantic::tool{"Planting advice for one crop"}]]
std::string advice(Crop crop);
}

// A toolbox. Each annotated member function is a tool bound to one object.
class Greenhouse {
public:
  [[= scrydantic::tool{"Read the temperature and the roof vents"}]]
  Climate climate() const;
  [[= scrydantic::tool{"Open or close the roof vents"}]]
  Climate set_vents(bool open);
  [[= scrydantic::tool{"Water one bed"}]]
  scrydantic::Status water(WaterArguments arguments);

  double liters() const; // no annotation, thus not a tool
};

auto house = std::make_shared<Greenhouse>();
scry::ToolRegistry tools;
tools.add(house);               // each add() returns scry::Status: examine it
tools.add<^^almanac>();
tools.add<^^to_fahrenheit>();
```

Each `add()` registers all of its tools or none of them. A registry is
additive: Scry rejects a duplicate name, and nothing removes a tool. The
full program is [examples/toolbox.cpp](../examples/toolbox.cpp). Asked to
cool the greenhouse, water bed 2, and give the temperature in Fahrenheit, it
printed:

```
The temperature in the greenhouse is currently at 21°C. After setting the roof vents to open, the temperature has now increased to 21°F. I've also watered bed 2 with 5 liters of water. Adjustments are being made to cool the greenhouse.
liters poured: 5
```

The model called three tools, and the host read the shared toolbox after the
turn. Run the program with `--tool-manifest` to print every generated schema
as one JSON document. This needs no model and no network. The manifest also
comes from `ToolRegistry::to_json()` in your own code.

A registry is a plain value. `Harness::create(config, std::move(tools))`
adopts it, and `harness.tools()` keeps it open for later registrations. A turn
uses the tools that were registered when `send()` accepted it.

### Parameters and results

A tool function can have one of these parameter lists:

- **No parameters.** The tool takes `{}`.
- **Plain parameters.** The argument object has one required member for each
  parameter. `set_vents(bool open)` takes `{"open": true}`. Each parameter
  must have a name, and GCC 16 cannot read annotations on parameters, so this
  form has no descriptions or defaults.
- **One struct.** The struct is the argument object. Use this form for
  descriptions, default values, optional members, and constraints.

A tool can return a supported value, a `scrydantic::Result` of one, `void`, or
`scrydantic::Status`. `void` and a successful `Status` send `{}` to the model.

These rules are Scrydantic's, and its guide has them in full: the
[types](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md#models)
that a member, a parameter, or a result can have, which members are required,
and the [annotations](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md#annotations)
that describe a member (`description`), rename it (`name`), bound it (`range`,
`length`, `items`, ...), or check it (`check`). All of them are in the
`scrydantic` namespace.

Scry adds one thing to a tool's signature. Before its other parameters, a tool
can take `const scry::ToolCallContext&`. The context gives the turn, the
round, the call ID, and the tool name, and it is not part of the schema. Its
string views are valid only until the handler returns.

```cpp
[[= scrydantic::tool{"Water one bed"}]]
scrydantic::Status water(const scry::ToolCallContext& call, WaterArguments arguments);
```

### What the model and the host see of a rejected value

A constraint bounds a member in the schema that the model sees and in the
decode, so the handler never sees a value past it. A validator checks what a
schema cannot express:

```cpp
scrydantic::Status known_city(const std::string& city) {
  if (city == "Oslo" || city == "Lima")
    return {};
  return std::unexpected(
      scrydantic::tool_error("must be a known city", "has no row in cities"));
}

struct Booking {
  [[= scrydantic::check<&known_city>]] std::string city;
  [[= scrydantic::range{.min = 1, .max = 8}]] std::int32_t guests;
};
```

When the model calls a tool that takes a `Booking` with
`{"city":"Rome","guests":9}`, your function does not run. Scry answers the
model with both problems as the tool result, one line each: `$.city must be a
known city` and `$.guests must be at most 8`. The turn continues, and the
model can call again.

The host gets the same problems as data. `on_tool_call` receives the error in
`ToolCall::error`, a `scry::Error` of category `tool`. Its `issues` lists each
problem as a `scrydantic::Issue`, with its kind, its path, and its reason. Its
`message` has the lines for the host, here `$.city has no row in cities`.
Scry never sends that text to the model. Typed answers follow the same rules:
[`ask<Answer>()`](#get-a-typed-answer) sends a rejected answer back to the
model in the same way.

## Refuse a call, and let the model try again

Return `std::unexpected(scrydantic::tool_error(model_text, host_text))` from
a tool. The model reads the first text as the tool result and can correct
itself. The second text stays in `Error::message` on the host, and Scry never
forwards it; an `on_tool_call` observer reads it in `ToolCall::error`. The turn
continues.

```cpp
[[= scrydantic::tool{"Step one room in a direction"}]]
scrydantic::Result<WalkResult> walk(const WalkArguments arguments) {
  if (arguments.direction != Direction::north) {
    return std::unexpected(scrydantic::tool_error(
        "a wall blocks that direction; only north is open from the entrance hall",
        "player attempted a blocked walk from the entrance hall"));
  }
  return WalkResult{.room = "library"};
}
```

[examples/tool_policy.cpp](../examples/tool_policy.cpp) observes each call in
`on_tool_call`. Asked to walk east, it printed:

```
refused walk: {"error":"a wall blocks that direction; only north is open from the entrance hall"}
It looks like you'll have to walk towards the north instead. There's a wall blocking your way to the east from the entrance hall.
```

An unknown tool name, arguments that fail the generated schema, a handler
that throws, and a refusal all reach the model the same way: as
`{"error": ...}` with `ToolCall::is_error` set. Scry forwards only
`model_message`, never exception text. Argument errors name the JSON path and
what the schema required there. For `{"direction":"up"}`, the model reads
`$.direction is not a declared enumerator; must be one of: north, south, east,
west`.

## Control which calls run

Three settings bound a turn. All of them answer the model instead of failing
the turn, so the history still commits.

- `Config::max_tool_calls_per_turn` refuses every call past the limit, across
  all rounds. One response can request many calls, so `max_tool_rounds` alone
  cannot bound this.
- `TurnCallbacks::on_tool_request` runs before each handler. Return
  `std::nullopt` to admit the call, or a `scry::ToolRejection` with a
  `model_message` to refuse it. A refused call runs no handler, so it has no
  side effect.
- `Config::max_tool_rounds` (default 8) limits the loop. Under the default
  `ToolRoundLimitPolicy::fail`, a response that asks for one more round fails
  the turn with `max_tool_rounds` and commits nothing. Under
  `ToolRoundLimitPolicy::complete`, the turn ends with the text of that
  response, the finish reason `tool_round_limit`, and the dropped calls in
  `Completion::unexecuted_tool_calls`.

```cpp
scry::ToolAdmissionCallback echo_once_per_turn() {
  return [echo_calls = 0](const scry::ToolRequest& request) mutable
      -> std::optional<scry::ToolRejection> {
    if (request.context.tool_name != "echo" || echo_calls++ == 0)
      return std::nullopt;
    return scry::ToolRejection{
        .model_message = "echo may be called once per turn; answer with what you already have"};
  };
}
```

To finish a turn without more tools, set a flag from a handler or from
`on_tool_call` and refuse every later request. Do not cancel: a cancelled turn
rolls back its transcript, but the host state that executed handlers changed
stays. Tool side effects are never transactional. The host owns rollback and
idempotency. [examples/main_loop.cpp](../examples/main_loop.cpp) uses all
three settings.

## Register a tool that has no C++ declaration

`add_dynamic()` takes a `ToolDefinition` with a hand-written JSON schema and a
handler of `Json -> Result<Json>`, or `(const ToolCallContext&, Json) ->
Result<Json>`. Scry checks only that the arguments parse as a JSON object. The
handler validates them against its schema and refuses with `scry::tool_error()`.

```cpp
tools.add_dynamic(
    scry::ToolDefinition{
        .name = "echo",
        .description = "Return the supplied text unchanged",
        .input_schema = {.text = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})"},
    },
    [](const scrydantic::Json& arguments) -> scry::Result<scrydantic::Json> {
      const auto root =
          scrydantic::JsonView::parse(arguments).value_or(scrydantic::JsonView{});
      const auto text = root.find("text");
      if (!text || text->kind() != scrydantic::JsonKind::string)
        return std::unexpected(scry::tool_error("echo requires a string text property"));
      return scrydantic::Json{.text = R"({"echo":)" + scrydantic::escape_json_string(*text->string()) + "}"};
    });
```

`scrydantic::JsonView::parse()` reads JSON, and
`scrydantic::escape_json_string()` writes a JSON string with the canonical
escapes. [examples/main_loop.cpp](../examples/main_loop.cpp) has the complete
handler.

When a C++ struct does describe the arguments, or a part of them, the handler
can decode it with Scrydantic and forward a failure with `scry::tool_error()`.
The model then gets the same lines as for a reflected tool, each with the path
of the field, and `ToolCall::error` holds the issues for the host:

```cpp
struct EchoArguments {
  std::string text;
};

[](const scrydantic::Json& arguments) -> scry::Result<scrydantic::Json> {
  auto echo = scrydantic::decode<EchoArguments>(arguments);
  if (!echo)
    return std::unexpected(scry::tool_error(std::move(echo.error())));
  return scrydantic::Json{
      .text = R"({"echo":)" + scrydantic::escape_json_string(echo->text) + "}"};
}
```

## Get a typed answer

`ask<Answer>()` makes the model answer with a C++ value instead of text. The
schema, the strict decoder, and the correction text all come from the type.
If the model gives an incorrect shape, or a value that a constraint or a
validator of `Answer` rejects, Scry sends the error text back and the model
tries again. The decode rules are the same as for tool arguments, so
`Answer` must be a plain aggregate.

```cpp
struct Verdict {
  [[= scrydantic::description{"Is the claim supported by evidence?"}]] bool supported{};
  [[= scrydantic::description{"One sentence explaining the verdict"}]] std::string reason{};
};

auto verdict = harness->ask<Verdict>(*conversation, "Is the moon made of cheese?");
if (verdict)
  std::cout << verdict->value.supported << ' ' << verdict->value.reason << '\n';
```

`ask()` blocks. `send<Answer>()` is the form for a main loop. Its
`on_finished` receives the same `Answered<Answer>` that `ask()` returns: the
decoded `value`, and the `completion` with the text, the usage, and the counts.

```cpp
auto turn = harness->send<Verdict>(*conversation, "Is the moon made of cheese?", {
    .on_finished = [](scry::Result<scry::Answered<Verdict>> done) {
      if (done)
        std::cout << done->value.reason << '\n';
    },
});
// ... call harness->update() from your loop until turn->finished()
```

The value is the one that Scry already decoded to accept the answer. Do not
decode `completion.structured` again to get it: a second decode runs the
`scrydantic::check` validators of `Answer` again, after the history has
committed.

For a response tool with another name, another description, or a hand-written
schema and validator, pass a `ResponseFormat` to `send_structured()` or
`send_and_wait_structured()`. These deliver the answer as canonical JSON in
`Completion::structured`. [examples/typed_answer.cpp](../examples/typed_answer.cpp)
uses `ask()` and a hand-written format. It printed:

```
not supported: The moon is not made of cheese. It is a natural satellite of the Earth, composed primarily of rock and dust.
score: {"score":0}
```

A typed turn is the ordinary tool loop plus one response tool. The model can
still call your tools first. The committed history holds the answer as a text
block, so a restored conversation works with both dialects.

### See what the model is sent, without a model

A typed answer and a tool's arguments go through Scrydantic's schema generator
and strict decoder. Both work without a Harness, so you can print the schema of
a type, and the text that a model gets for an incorrect value, in a unit test
or a small program. [examples/typed_values.cpp](../examples/typed_values.cpp)
includes only `<scrydantic/scrydantic.hpp>`. It declares a tagged variant, a
`skip_null` class, and two constraints, and prints:

```
schema: {"additionalProperties":false,"properties":{"actions":{"items":{"anyOf":[{"additionalProperties":false,"properties":{"priority":{"enum":["low","high"],"type":"string"},"text":{"description":"What to remind the user of","minLength":1,"type":"string"},"type":{"enum":["reminder"],"type":"string"}},"required":["text","type"],"type":"object"},{"additionalProperties":false,"properties":{"body":{"type":"string"},"type":{"enum":["note"],"type":"string"}},"required":["body","type"],"type":"object"}]},"maxItems":5,"type":"array"},"summary":{"anyOf":[{"type":"string"},{"type":"null"}]}},"required":["actions"],"type":"object"}
round trip: {"actions":[{"priority":"high","text":"stretch","type":"reminder"},{"body":"water the plants","type":"note"}]}
rejected:
$.actions[0].type must be one of: reminder, note
$.actions[1].text must have at least 1 character
issue at $.actions[0].type
issue at $.actions[1].text
```

The lines after `rejected:` are `scrydantic::Error::model_message` for a decode
of `{"actions":[{"type":"alarm","text":"now"},{"type":"reminder","text":""}]}`,
one line for each issue. In a turn, Scry sends this text to the model as it
is. The last two lines print the path of each entry of `Error::issues`.

For the tools of a registry, `ToolRegistry::to_json()` gives every schema in
one document. For everything else that Scrydantic does alone, such as serving
the same annotated functions over MCP, refer to
[its repository](https://github.com/crybo-rybo/scrydantic).

## Drive a turn from a main loop

`send()` validates the request, snapshots the tools, and returns at once with
a `Turn` handle. Scry does the network work on one worker thread per Harness.
Nothing of yours runs until you call `update()`, and then everything runs on
the calling thread: text deltas, tool handlers, `on_tool_request`, answer
validators, and `on_finished`. Your tools need no locks.

```cpp
auto turn = harness.send(conversation, "Is the main loop running?", {
    .on_text_delta = [](std::string_view text) { std::cout << text; },
    .on_tool_call = [](const scry::ToolCall& call) { /* observe */ },
    .on_finished = [&](scry::Result<scry::Completion> done) { /* once */ },
});

while (app.running() && !turn->finished()) {
  const auto stats = harness.update({
      .time_budget = std::chrono::milliseconds{2},
      .max_callbacks = 32,
  });
  if (stats.callbacks_delivered == 0 && stats.events_remaining == 0)
    std::this_thread::sleep_for(std::chrono::milliseconds{1}); // no frame to ride
}
```

- **The time budget is soft.** `update()` checks it between events and
  callbacks. It cannot interrupt a handler. It always makes one unit of
  progress, so a small budget slows the pump and never starves it.
- **`on_finished` runs once**, with the `Completion` or with the terminal
  `Error`, including `cancelled`. It does not run if you disconnect the turn
  or destroy the Harness first.
- **History commits inside `update()`**, when the terminal event is
  processed, before `on_finished`. A failed or cancelled turn commits nothing.
- **Many conversations share one Harness.** Their turns run in first-in,
  first-out order. Each conversation has at most one queued or active turn;
  a second `send()` on it fails with `busy`.
- **`send_and_wait()` and `ask()`** pump `update()` for you until the turn
  ends. Use them in scripts and tests. They also run the callbacks of other
  accepted turns, and they fail with `invalid_state` from inside a callback.

| Operation | Effect |
| --- | --- |
| Drop the `Turn` | Nothing. The turn continues and its callbacks still run. |
| `turn.cancel()` or `harness.cancel(id)` | Request cooperative cancellation. `on_finished` gets `cancelled`. A tool that already runs is not interrupted. |
| `turn.disconnect()` or `harness.disconnect(id)` | Clear every callback. The turn runs on and still commits or rolls back. |
| Destroy the `Harness` | Stop the worker, join it, and discard undelivered callbacks. |

Destroy the Harness before the objects its handlers and callbacks capture.
[examples/main_loop.cpp](../examples/main_loop.cpp) is the complete program,
with a dynamic tool, an admission hook, and a print of the history.

## Configure the provider

`scry::Config` is a designated-initializer value. `Harness::validate(config)`
runs the checks of `create()` without libcurl or a worker, for a settings
dialog.

```cpp
const scry::Config config{
    .base_url = "https://api.anthropic.com",
    .api_key = key,
    .model = "claude-sonnet-5-5",
    .dialect = scry::ProviderDialect::anthropic,
    .sampling = {.temperature = 0.2, .max_tokens = 2048},
    .max_tool_rounds = 4,
    .tool_round_limit = scry::ToolRoundLimitPolicy::complete,
    .extra_headers = {{.name = "x-team", .value = "render"}},
};
```

| Setting | Anthropic Messages | OpenAI-compatible Chat Completions |
| --- | --- | --- |
| `base_url` | An origin, or the full `/v1/messages` endpoint | An origin, a `/v1` base, or the full `/v1/chat/completions` endpoint |
| `api_key` | Required, sent as `x-api-key` | Optional bearer token |
| `sampling.temperature` | 0 to 1 | 0 to 2 |
| `sampling.max_tokens` | Required. Default 1024 | Optional. Unset omits the field |
| `sampling.seed` | Rejected | Sent when set. Repeatability is the promise of the server, not of Scry |
| `reasoning_mode = disabled` | Rejected | Sends `reasoning_effort: "none"` |

Both dialects stream. Scry retries network failures, rate limits, and HTTP
5xx responses with exponential backoff and jitter, and it obeys
`Retry-After`. It retries only before any text or tool call arrived. `retry`,
`timeouts`, and `limits` hold the bounds, each with a default that
[config.hpp](../include/scry/config.hpp) documents. A local server can take
minutes before its first token: raise `timeouts.idle` for it. For a private
CA, set `ca_bundle_path`; for a proxy, set `proxy`. Scry verifies TLS peers
by default.

## Handle errors

Each Scry call that can fail returns `scry::Result<T>`, which is
`std::expected<T, scry::Error>`, or `scry::Status`. `Error::category` is the
programmatic reason, `message` is the host diagnostic, and `retryable` says if
a new request can succeed. Scry sanitizes provider text and removes the API
key from diagnostics.

`scry::Error` is not `scrydantic::Error`, which has only the texts and the
issues of a codec failure. You get a `scrydantic::Error` only from a
Scrydantic function that you call yourself, such as `scrydantic::decode<T>()`.
When Scry reports a failure that began in Scrydantic, it copies the texts and
the issues into a `scry::Error` and sets the category. `scry::tool_error()`
does the same for a `scrydantic::Error` that your own callable holds.

| Category | Meaning |
| --- | --- |
| `invalid_config` | A configuration value or a serialized document is not valid |
| `invalid_state` | The operation is not valid for the current state, for example a moved-from handle |
| `invalid_argument` | A caller argument is not valid, including a duplicate tool name |
| `busy` | The conversation already has an accepted turn |
| `authentication`, `rate_limit`, `network`, `protocol` | What the provider or the transport reported |
| `resource_limit` | An admission or payload bound was exceeded |
| `tool` | Tool arguments, dispatch, or result encoding failed |
| `max_tool_rounds` | The model asked for more rounds than allowed, under the `fail` policy |
| `cancelled` | The turn was cancelled |

Before acceptance, `send()` returns the error directly. After acceptance,
`on_finished` is the only channel. Check `Completion::finish_reason` when the
application needs an answer that is not truncated: `length` means the token
limit cut the response.

## Save and restore a conversation

```cpp
auto saved = conversation.to_json();            // Result<scrydantic::Json>
auto restored = scry::Conversation::from_json(*saved);
```

`to_json()` writes the system prompt and the committed messages as a canonical
versioned document. It never includes an active turn. `from_json()` rejects
malformed JSON, unknown fields or versions, and invalid roles or blocks with
`invalid_config`. Scry does no file I/O; the host owns storage.
`conversation.messages()` gives the committed history as `scry::Message`
values, each with text, tool-call, and tool-result blocks.

## Test without a model

`scry::testing` is an optional package component. It gives a scripted HTTP
server on `127.0.0.1`. The test sets `Config::base_url` to the URL of the
server and creates the Harness with the ordinary `Harness::create`. Thus the
test runs all of Scry, libcurl included, and needs no model and no network.

```cmake
find_package(scry CONFIG REQUIRED COMPONENTS testing)
target_link_libraries(my_tests PRIVATE scry::scry scry::testing)
```

```cpp
#include <scry/scry.hpp>
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>

auto server = scry::testing::ScriptedServer::create();
server->enqueue({.body_chunks = {scry::testing::anthropic_tool_stream({
    {.id = "call-1", .name = "read_sensor", .arguments = R"({"room":"attic"})"}})}});
server->enqueue({.body_chunks = {scry::testing::anthropic_text_stream("The attic is at 19C.")}});

auto config = my_config();
config.base_url = server->url();
auto harness = scry::Harness::create(config);
// ... send a turn, pump update(), then assert on what the application saw and
// on server->requests(): method, target, headers, and body of each request.
```

`<scry/testing/streams.hpp>` builds streamed bodies for both dialects. A
response can hold before its first byte or pause after some chunks until the
test calls `release()`, and it can close the connection early to make a
retryable `network` failure. For fast retries in a test, set
`config.retry.jitter_ratio` to 0 and millisecond backoffs. For a fast
cancellation, set a low `config.timeouts.shutdown`.

[examples/testing_scripted.cpp](../examples/testing_scripted.cpp) is a complete
test in this form, without a test framework. The test suite of this repository
runs it. [tests/testing/](../tests/testing/) has the
equivalent with Catch2. [scripted_server.hpp](../include/scry/testing/scripted_server.hpp)
documents the script.
