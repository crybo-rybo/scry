# Scry architecture

Scry is a static C++ library for applications that own their main loop. It sends
requests to an LLM server, streams text, and executes tools. It sends the tool
results back to the server, and it commits the conversation history when a turn
succeeds. The host calls `Harness::update()` to run tools and to deliver
callbacks on the host thread.

This document states what Scry guarantees and where its limits are. For a
complete program, see [README.md](../README.md). For usage by task, see
[guide.md](guide.md). The public headers under `include/scry/` define the API.

## What Scry owns, and what Scrydantic owns

Scry is built on [Scrydantic](https://github.com/crybo-rybo/scrydantic), a
separate library with its own repository, releases, tests, and documentation.
The boundary is this:

| Scrydantic owns | Scry owns |
|---|---|
| The JSON parser and the canonical writer: `Json`, `JsonView` | The registry of tools, and the snapshot of it that each turn uses |
| The annotations, and the rules for which C++ types have a JSON form | The call context, `ToolCallContext`, that a tool can take |
| The JSON Schema of a type, `schema_v` and `input_schema_v` | Which schemas go to the model, and in which provider envelope |
| Strict decoding, with constraints, validators, and the text and the issues of a failure | When a call runs: the limits, the admission hook, and the host thread |
| Encoding a value as canonical JSON | What the model receives for a failure, and the byte limits on arguments and results |
| The binding of an annotated function, a namespace, or a toolbox into a handler of `Json -> Result<Json>` | The turn loop, retries, the provider dialects, the transport, and the history |

Scry uses only Scrydantic's public API. It has no JSON parser, codec, or schema
generator of its own, and it links no other JSON library. Scry's public API
uses Scrydantic's types where a value is JSON or a declaration is reflected:
`scrydantic::Json` in tools, messages, and completions, `scrydantic::Issue` in
`scry::Error`, and the `scrydantic::` annotations on your own types.
`<scry/scry.hpp>` includes `<scrydantic/scrydantic.hpp>`.

Thus this document does not restate Scrydantic's contract. The
[Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md) is
the reference for the supported types, the annotations, the constraints and
validators, the schema subset, the decoding rules, and the JSON text that the
parser accepts and the writer produces. Where a behavior below depends on one
of them, this document names it and says what Scry adds.

## Public API

| Type | Responsibility |
|---|---|
| `Config` | Provider endpoint, credentials, model, sampling, retries, timeouts, and resource limits |
| `Conversation` | System prompt and committed message history |
| `ToolRegistry` | Standalone additive registry of reflected and dynamic tools. A Harness takes ownership of it at `create()` |
| `Turn` | Handle to an accepted exchange: identity, completion query, cancellation, and callback disconnection |
| `ResponseFormat` | Response tool and answer validator that make a turn end with a [structured answer](#typed-completions) |
| `Harness` | Configured runtime, worker thread, registry, and callback pump |

`Harness::validate(config)` runs the configuration checks that `create()` uses.
It does not initialize libcurl, and it does not start a worker. A successful
validation does not guarantee that the runtime initialization succeeds.
Operations that can fail return `Result<T>` (`std::expected<T, Error>`) or
`Status` (`Result<void>`).

`send()` validates admission and returns without a wait for network I/O. It
rejects these inputs:

- Empty user text, with `invalid_argument`.
- An inactive handle, with `invalid_state`.
- A busy Conversation, with `busy`.
- A request that exceeds an admission limit or the Conversation payload limit,
  with `resource_limit`.

Each Conversation has a maximum of one queued or active turn. Different
Conversations can queue turns on the same Harness.

Scry does not load model weights, and it does not own the main loop of the
application. Its public extension points are configuration and tool callables.
The provider and transport interfaces are internal.

## Threading and lifetime

Use a Harness, its registry, its Turns, and their Conversations from one host
thread. The public handles do not synchronize concurrent access by the
application. A registry that no Harness owns yet is a plain move-only value on
the thread that built it. `Harness::create()` adopts the registry and makes the
source inactive.

All callbacks, tool handlers, and answer validators run inside `update()` on
the thread that calls `update()`. Thus they can directly access host state that
this thread owns.

One worker per Harness owns the network transfers, the provider decoding, and
the turn machine. The worker processes accepted turns in FIFO order, with one
active turn at a time. Later turns wait while the active turn retries or waits
for tool results. Each Harness instance has its own worker and its own runtime
state.

```mermaid
flowchart LR
    App["Host: send / cancel"] --> Commands["Command queue"]
    Commands --> Worker["Worker: turn machine / provider / curl"]
    Worker --> Events["Event queue"]
    Events --> Pump["Host: update"]
    Pump --> Callbacks["Callbacks and tool handlers"]
    Callbacks --> Commands
```

The command queue and the event queue use mutexes. Per-turn cancellation uses an
atomic flag. Harness shutdown uses the stop token of the worker. Accepted
requests share immutable snapshots of the history and of the schemas. If a
snapshot still holds the history, Scry copies the history before it changes it.
Callbacks, tool handlers, and the mutable Conversation state stay on the host
side.

`update()` ingests worker events, processes terminal state, and delivers pending
callbacks and tool calls. Its time budget is soft. `update()` checks the budget
between events and callbacks, and it cannot interrupt a handler. When work is
available, it ingests at least one event before it checks the time. It also
delivers at least one pending callback or tool call if `max_callbacks` allows
it. One tool dispatch counts as one unit against that limit, and this unit
includes the optional `on_tool_call` observer.

`max_callbacks = 0` prevents dispatch and callback delivery. But terminal events
can still commit history and clear the busy state. Route cleanup runs on every
update. When the budget runs out, `update()` defers only the release of
discarded events.

The event queue coalesces adjacent text events from the worker, and the pump
combines the pending text for each turn. The byte count of the queue includes
the events that the pump keeps until it delivers or discards them. Coalescence
decreases the callback traffic. The configured payload limits supply the byte
bound.

Callbacks can call `send()`, `cancel()`, and `disconnect()`, and they can
register tools. A reentrant `update()` does no work and returns
`budget_exhausted = true`. If an observer callback throws an exception, the
exception propagates out of `update()` after that event counts as delivered. The
Harness stays valid. Scry catches tool-handler exceptions and converts them into
tool-error results.

`send_and_wait()` runs `send()` and then pumps `update()` until the requested
turn finishes. `ask<Answer>()` does the same for a typed turn. These functions
also run callbacks and handlers for other accepted turns. `send_and_wait()`
does not expose the Turn handle of the turn that it waits for. If a callback or
a handler calls `send_and_wait()`, it returns `invalid_state`. If `update()`
throws while `send_and_wait()` pumps it, the exception propagates out of
`send_and_wait()`, and `send_and_wait()` disconnects the waited turn. That turn
continues to run and still commits its history.

| Operation | Effect |
|---|---|
| Drop a `Turn` | Work and callbacks continue. Destruction does not wait |
| `Turn::cancel()` or `Harness::cancel(id)` | Request cooperative cancellation. The terminal callback stays attached |
| `Turn::disconnect()` or `Harness::disconnect(id)` | Clear callbacks. Tool processing and history processing continue |
| Destroy a `Harness` | Request worker shutdown, join the worker, clear the busy state, and discard undelivered callbacks |

If a cancellation occurs before a queued turn starts, the turn sends no network
request. Scry cannot interrupt a tool that runs. If Scry sees the cancellation
after the tool returns, Scry suppresses the result of that tool and the other
calls that remain. Cancellation does not reverse a terminal outcome that the
worker already produced.

A disconnect inside a callback applies to the subsequent deliveries. The
callback that executes stays alive until it returns or throws. If a tool handler
disconnects, Scry also suppresses the `on_tool_call` observer for the call of
that handler. A disconnect does not cancel tools. The host must continue to pump
until the turn finishes.

`Turn::finished()` becomes true after the delivery of the terminal callback. If
no terminal callback is attached, it becomes true after terminal processing.
When a turn is finished, the runtime releases its callbacks and its tool
snapshot, even if the host still keeps a handle. The handle then reports only
identity and status. Moved-from handles and handles whose Harness no longer
exists also report true. A drop of a Conversation handle does not cancel an
accepted turn, because the runtime keeps the state of the Conversation.

Harness destruction waits for the worker. The transport checks for shutdown
between curl operations. It uses `timeouts.shutdown` to limit each curl poll
wait. This setting is not a timed join, and it is not a hard wall-clock deadline
for the destructor.

## Turn processing and retries

The turn machine does no I/O and does not read a clock. It consumes explicit
events and emits commands that the worker executes. Its states are queued, awaiting model, streaming, retry wait, awaiting tool, and terminal.
If a transition is not valid, the machine returns diagnostics. It does not
change state, and it does not issue work. Tests inject time and event sequences
directly.

A successful model response completes the turn or starts a tool round. In a
[typed turn](#typed-completions), the model completes the turn when it calls
the response tool. The round that holds that call ends the turn when the host
accepts the answer. Scry admits the calls from one response to the event queue as one batch. If the full
batch cannot fit, no handler in that batch runs. The pump dispatches the calls
in provider order. For each call, it posts the result to the worker and invokes
the optional `on_tool_call` observer. The worker sends the request again when
all the results are ready.

A fatal failure of dispatch or of the payload budget suppresses the later
handlers in the batch.

Tool-call IDs must be unique in a turn. Scry rejects reused IDs and does not
execute them again. `Config::max_tool_rounds` sets the limit of the loop.
`Config::tool_round_limit` sets what occurs when a response asks for one round
more than that limit. Under `ToolRoundLimitPolicy::fail`, which is the default,
the turn fails with `max_tool_rounds` and commits nothing. Under
`ToolRoundLimitPolicy::complete`, the turn stops but does not fail:

- Scry commits the rounds that ran and the text of the final response.
- Scry removes the tool-call blocks of that response and puts them in
  `Completion::unexecuted_tool_calls`.
- The finish reason is `tool_round_limit`.

If the response has no text after this removal, Scry commits no assistant
message. Thus the transcript ends with the tool results of the previous round.

Scry automatically retries network failures and rate-limit failures that are
retryable, including HTTP 5xx responses. It does this only before it consumes
semantic output. Text or tool-call content disables the retry, even if no text
callback ran. After partial output, a failure ends the turn. The `retryable`
flag of the failure still shows if a new request can succeed. Scry does not
retry authentication failures or protocol failures.

Backoff is exponential with jitter, and each Harness seeds its jitter
independently. A valid `Retry-After` can increase the delay, but `max_backoff`
limits the final delay. The retry limits for attempts and for elapsed time reset
for each model request in the tool loop. The elapsed-time limit controls retry
eligibility and wake times, but it does not stop an active transfer.
`Completion::attempt_count` is the total of the attempts across the turn, and
`usage` accumulates the usage from completed model responses.

## Tools

The host declares tools in C++ and registers them with `ToolRegistry::add()`,
which takes one of these:

- A toolbox object.
- An annotated function, or a namespace of annotated functions.
- An argument aggregate with a callable.

Scrydantic generates the argument schema of each tool, its strict argument
decoder, and its result encoder from the declaration. Scry stores the result as
a registry entry. [Dynamic tools](#dynamic-tools) are for tools that exist only
at runtime. The host registers them with `add_dynamic()`.

A registry is a standalone value. A host builds a registry, and
`Harness::create(config, std::move(tools))` adopts it. `Harness::tools()` keeps
the registry open for later registrations.

A registry is additive. Scry rejects duplicate names, and there is no operation
that replaces or removes a tool. Each registration call is atomic. A call that
registers many tools, such as a toolbox or a namespace, first validates all of
them:

- Each name must not be empty. It must be different from the other names in the
  call and from each tool that is already registered.
- Each schema must be a JSON object.
- Each handler must not be empty.

Only after these checks does the call insert the tools. Thus a failed call
leaves the registry exactly as it was. This is also true when the failure is an
allocation failure.

Each accepted turn keeps the registrations that were visible at `send()`. Scry
uses the same immutable snapshots of the registrations and the schemas again
until the host adds another tool. Handlers stay on the host thread.

Each tool handler, reflected or dynamic, has two possible shapes. One shape
receives only its arguments. The other shape also receives a `ToolCallContext`
that identifies the call that the handler services. The context has these
fields:

- The `TurnId`.
- The `call_id` that the provider assigned.
- The registered `tool_name`.
- The one-based `round`.
- The zero-based `index` in the batch of that round.

These values are the same values that the later `on_tool_call` observation
carries. Thus a handler can correlate its work with the turn, and it does not
have to count calls. The context is borrowed. Its two string views point into
the call block that Scry dispatches, and they are valid only until the handler
returns. If a handler keeps one of them after it returns, the handler must copy
the text. Internally, a registration stores one handler shape. Thus each
registration form exports the same kind of tool contract, and the model sees no
difference.

Before a handler runs, a call goes through two admission gates in a fixed order:

1. `Config::max_tool_calls_per_turn`. This limit applies to the calls that one
   turn can dispatch across all rounds. `max_tool_rounds` alone cannot do this,
   because one response can request many calls.
2. `TurnCallbacks::on_tool_request`, which is the policy of the host. It
   receives a `ToolRequest` that identifies the call and its canonical
   arguments. It returns nothing to admit the call, or a `ToolRejection` to
   refuse it.

Each call that gets to the route counts against the limit. This includes a call
to a tool that nobody registered, because the model used the budget of the turn
when it asked. Dispatch refuses an unknown tool with its own message. An unknown
tool never gets to the hook, because there is no handler for the host to admit.
A refused call runs no handler, so it cannot have a side effect.

The model gets `{"error": message}`, flagged as a tool error. The message is one
of these:

- For the limit, the fixed text `tool call limit for this turn reached; respond
  without calling tools`.
- For a hook refusal, the `model_message` of the host.

A typed turn still requires a tool call, and it ends only on its response tool.
Thus, in a typed turn, the text for the limit is different. It tells the model
to call the response tool, by its name, alone, with the final answer.

That result gets to `on_tool_call` with `is_error`. Scry posts it to the worker
like any other result, so the turn continues and commits normally. If the hook
throws, Scry handles it exactly like a handler that throws. Scry refuses the
call with the same fixed text as for a handler that throws, and the turn
continues. The hook runs under the same
invocation guard as all other callbacks. Thus a `disconnect()` from inside the
hook is deferred until the call returns, and it suppresses the `on_tool_call`
observation of that call.

If a hook cancels the turn, Scry obeys the cancellation before the handler runs.
This is true if the hook admits the call and if it refuses the call. Scry reads
the cancel flag again immediately when the hook returns. If the flag is set, no
handler runs, Scry posts no result, and no `on_tool_call` fires. The turn then
ends through the ordinary cancellation path of the worker. A handler that runs
at this point is the one thing that the rollback cannot repair for the host. The
reason is that the effects of a handler on host state stay after the turn
discards its transcript.

This check does not change the counts. The call still used one unit of the
per-turn limit. But a call that cancellation suppressed is not a refusal, and
Scry does not count it as one.

Tool side effects are not transactional. If a turn fails or is cancelled, the
Conversation history does not change, even if a handler already changed host
state. The host owns rollback, idempotency, and reconciliation for such effects.
For this reason, a cancel from inside a handler is rarely what a host wants. The
cancel discards the pending transcript, so Scry discards the results of the
executed tools and the full turn rolls back. But the side effects that those
handlers already had on host state stay.

If a host wants the turn to finish but wants no more tools to run, it sets its
own flag. It can set this flag from the handler or from `on_tool_call`. Then it
refuses every later request from `on_tool_request`. The model gets the reason,
the turn completes, and the history commits.

`ToolRegistry::to_json()` exports a version-1 JSON manifest. The manifest has a
`tools` array in registration order. Each entry has the registered `name`,
`description`, and `input_schema` object, and the schema includes reflected
parameter annotations. This manifest is the provider-neutral tool contract.
Provider adapters apply their own wire envelopes.

The export includes reflected tools and dynamic tools. It invokes no
handlers and makes no provider request. It reads the current registry, not the
frozen snapshot of an active turn. It returns owned text that later
registrations do not change. If the text must go to a file, the host writes it.
If the export must run as a build step, the host runs it.

The export includes only the tools that the host registers on that execution
path. The export does not need a Harness. A registry that the host builds alone
exports the same document without provider configuration, libcurl, or a worker.
The manifest version is independent of the library version. Incompatible
changes to its structure or to the meaning of its fields increment the version.
Additive fields keep the version. We recommend that consumers ignore unknown
fields.

### Reflected tools

A host declares a reflected tool in one of three forms. Each form is one of
Scrydantic's binding functions, called with `scry::ToolCallContext` as the call
context, and each lowers to the same registry entry: a name, a description, an
input schema, and a handler.

| `ToolRegistry` call | Scrydantic binding | Registers |
|---|---|---|
| `add(std::shared_ptr<T>)` | `bind(toolbox)` | Each member function of `T` that has the `scrydantic::tool` annotation, bound to that one object |
| `add(T&&)` | `bind(toolbox)` | The same, after Scry moves the object into the registry. Thus the registry owns the only copy |
| `add<^^function>()`, `add<^^name_space>()` | `bind<^^entity>()` | One annotated function, or each annotated function that the namespace declares directly, in declaration order |
| `add<Args>(ToolMetadata, handler)` | `bind_handler<Args>()` | Any callable that takes `Args`, under a name and a description that the host supplies at runtime |

`examples/toolbox.cpp` shows each form. The
[Tools section of the Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md#tools)
has the rules for a tool function: the parameter lists that it can have, the
types of its parameters and its result, and the annotations. A declaration that
breaks a rule fails to compile at `add()`. A `static_assert` gives the function,
the member path, or the namespace, and the reason, as in `ForecastArgs does not
satisfy scrydantic::ToolArguments: ForecastArgs::window.when:
std::chrono::duration<long int> is not a supported reflected value`.

Scry adds these rules to a reflected tool:

- **The call context.** A tool function, or an `add<Args>()` handler, can take
  a `const ToolCallContext&` as its first parameter. In a member function with
  an explicit object parameter, the context comes immediately after it. The
  context is not part of the schema. A context in any other position is a
  compile error.
- **Names.** A tool has the name of its function, or the name that a
  `scrydantic::name` annotation gives it. The description is the text of the
  `tool` annotation. Tool names in one toolbox or namespace must be different
  from each other, which the compiler checks. Scry checks the names against the
  registry at registration.
- **Toolbox lifetime.** Each registration from a toolbox holds a copy of its
  `std::shared_ptr`. Thus the toolbox stays alive while the registry stays
  alive, or while a turn that has a snapshot of the registry stays alive. Scry
  releases the toolbox on the host thread. The registry never borrows an
  object: an lvalue passed to `add(T&&)` is a compile error. Instead, a host can
  pass a `std::shared_ptr` with a no-op deleter and own the lifetime. A null
  toolbox pointer fails with `invalid_argument`. A toolbox that is registered as
  `std::shared_ptr<const T>` admits only `const` member functions.
- **The thread.** Handlers run synchronously on the host thread, inside
  `Harness::update()`. Thus the state of a toolbox needs no locking.
- **What the model sees.** Scry sends the input schema of a tool to the
  provider. It never sends a schema for the result.
- **Results.** Scry checks each result against `max_tool_result_bytes` and
  then makes it canonical. A function that returns `void` or a successful
  `scrydantic::Status` sends `{}`.

Registration reads only the declarations that come before the call in its own
translation unit. Thus, declare the tools of a namespace before you register the
namespace. Two translation units that see different declarations each register
what they see: the `Tools` template parameter of each reflected `add()` records
that view, so leave it defaulted.

A reflected handler reports a failure as a `scrydantic::Error`, which has no
category. Scry reports it as `ErrorCategory::tool`, with the same `message`,
`model_message`, and `issues`:

- **The arguments do not decode.** `model_message` has one line for each issue
  that the decode found, up to `scrydantic::max_issues`. Each line gives the
  JSON path of the incorrect value and what the schema, a constraint, or a
  validator required at that path. `issues` holds the same issues as data. The
  model gets `model_message` alone. Each word of it comes from the schema that
  the model already has, from the input, or from the model text of a validator.
  Thus the model can correct itself, and it learns nothing new. If the argument
  text is not JSON at all, the text is `tool arguments are not valid JSON`.
- **The function refuses the call.** It returns
  `std::unexpected(scrydantic::tool_error(model_message, host_message))`.
  [Tool errors](#tool-errors) describes what the model gets.
- **The result does not encode.** Examples are a number that is not finite and
  a string that is not valid UTF-8. Such a failure describes the result type,
  and the model never sees the schema of that type. Thus it has no
  `model_message`, and the model receives the fixed diagnostic.

An `on_tool_call` observer gets each of these errors in `ToolCall::error`.

The public message model is also reflected. `TextBlock`, `ToolCallBlock`, and
`ToolResultBlock` carry the tags `text`, `tool_call`, and `tool_result`. Thus
`ContentBlock` is a tagged variant. A host can give a `Message`, or a vector of
them, to `scrydantic::encode()` and `scrydantic::decode()`. The encoding is the
shape of each message in the `Conversation::to_json()` document.

Each member of the model has an initializer. Thus `decode()` reads an absent
member as its initial value, but `Conversation::from_json()` requires each
member. The blocks carry annotations. Thus an include of `<scry/message.hpp>`
needs a C++26 compiler, as the rest of the public API does.

### Dynamic tools

`ToolRegistry::add_dynamic(ToolDefinition, ToolHandler)` registers a tool. It
takes a JSON schema object that the host writes and a move-only
`Json -> Result<Json>` callable. `add_dynamic(ToolDefinition, ContextualToolHandler)` accepts a
move-only `(const ToolCallContext&, Json) -> Result<Json>` callable. Use them
for tools that exist only at runtime, where there is no C++ declaration to
reflect. Examples are tools from a scripting language, another process, or a
plugin manifest. Register a tool that has a C++ declaration with `add()`. Then
Scry generates its schema and its argument checks, and they cannot become
different.

The arity of the handler selects the overload. Thus a lambda of either shape
selects one of the overloads without a cast. Registration validates the schema
as a JSON object and makes it canonical. Scry does not implement general JSON
Schema validation.

Registration rejects an empty handler of either shape. The handler receives
canonical object arguments, and it owns the validation against its schema. Scry
checks only that the arguments parse and form an object. Scry does not check
that they match the schema that the model received. If a handler rejects the
arguments with `scry::tool_error()`, the model gets the reason. The turn
continues, so the model can correct the call.

`on_tool_request` can do the same check before any handler runs. A handler must
synchronously return valid JSON or an error. Scry does not support asynchronous
or deferred tool results.

### Tool errors

Unknown tools, reflected decode failures, handler errors, exceptions, and
invalid result JSON produce bounded error results that the model can see. Scry
forwards the `model_message` of a handler error inside `{"error": ...}`, subject
to the byte cap for results. Scry does not forward its `message` or any
exception text. `scry::tool_error(model_message, host_message)` builds such an
error. If `model_message` is empty, Scry keeps its fixed diagnostic,
`tool handler returned an error`. If any error text, a `model_message`
included, is too large for the cap, Scry replaces it with the generic
`tool execution failed`.

A reflected handler refuses a call with `scrydantic::tool_error()`, which takes
the same two texts. Scry reports the error of a reflected handler, or of its
argument decoding or result encoding, as `ErrorCategory::tool`.

Reflected decode failures and unknown-tool errors carry `model_message` text
that comes from the schema. For an unknown tool, this text gives the requested
tool name and the registered tool names. The tool list of the request already
carried these names. Scry does not redact `model_message`. Thus, if a host puts
a secret in a `model_message`, the host publishes that secret. If a result is
too large, or if an error result cannot fit its bound, the turn fails with
`resource_limit`.

`on_tool_call` observes the canonical result and its `is_error` flag after Scry
posts the result to the worker. It does not confirm that the server received the
result. Cancellation or a fatal framework failure can suppress this observer.

### JSON values

Each JSON value in Scry's API is a `scrydantic::Json`, which owns serialized
text. Scry makes the text canonical, with Scrydantic's writer, before it keeps
it or gives it to the host. This applies to the arguments that a handler and
`on_tool_request` receive, the arguments and the result in a `ToolCall`, the
payloads in committed messages, `Completion::structured`, a registered schema,
and the documents that `to_json()` returns. Canonical text has these
properties:

- It has no insignificant whitespace, and its object keys are in lexical byte
  order.
- Duplicate object keys are merged into one key, and the last occurrence wins.
  Thus handlers do not see the original lexical duplicates.
- Each number and each string escape has one spelling. Thus equal values are
  equal bytes.

Text that Scrydantic's parser rejects is not JSON to Scry. This includes input
with more than 256 arrays and objects open at the same time, and strings that
are not valid UTF-8. The [Scrydantic guide](https://github.com/crybo-rybo/scrydantic/blob/main/docs/guide.md#working-with-raw-json)
and the contract in its
[`src/json/document.hpp`](https://github.com/crybo-rybo/scrydantic/blob/main/src/json/document.hpp)
give the accepted text and the canonical form in full.

A dynamic handler reads its arguments with `scrydantic::JsonView::parse()`.
`scrydantic::escape_json_string()` makes a quoted JSON string for a result that
the host builds by hand.

## Typed completions

A turn can end with a structured answer instead of free text. The answer is a
C++ type. Scrydantic supplies three things from the type: the schema that the
model gets, the strict decoder that checks the reply, and the error text for
corrections. Scry supplies the turn: it offers the model a response tool,
validates each candidate on the host thread, and commits the accepted answer.

`ask<Answer>(conversation, text)` blocks like `send_and_wait()`, and it returns
`Result<Answered<Answer>>`. This result holds the decoded `value` and the
`Completion` of the turn. The `Completion` carries what the value alone does
not: prose, usage, attempts, and counts. `ask()` returns the value that the
validator of the turn already decoded. Thus the `scrydantic::check` validators
of `Answer` run once for each candidate, and not again after the turn commits.

`send<Answer>(conversation, text, callbacks)` is the form for a host that polls.
Its `Completion::structured` holds the canonical JSON of the answer for
`scrydantic::decode<Answer>()`. `Answer` must satisfy `ToolArguments`, because
the tool input of a provider is always a JSON object. Any other type fails to
compile, and the diagnostic gives the member path and the reason, as for
`add<Args>()`.

A typed turn is the ordinary tool loop plus one response tool. A
`ResponseFormat` describes the response tool with these fields:

- `name`, which is `respond` by default.
- `description`. If it is empty, Scry uses its own instruction: call the tool
  exactly once, alone, with the final answer.
- `schema`, which is a JSON Schema object.
- `validate`, which is optional and runs on the host thread.

`scry::response_format<Answer>()`, in `<scry/response_format.hpp>`, pairs
`input_schema_v<Answer>` with a `decode<Answer>()` validator. The validator
reports a rejected answer as `invalid_argument`. `send<Answer>()` and `ask<Answer>()` use it. For a
different name, a different description, or a schema that the host writes, use
`send_structured(conversation, text, format, callbacks)` or
`send_and_wait_structured(conversation, text, format)`. These functions have
different names for two reasons:

- `send(conversation, text, {})` continues to mean "no callbacks".
- The compiler never selects `send<Answer>()` without its explicit template
  argument.

Before acceptance, these functions reject a format with `invalid_argument` in
these conditions:

- Its name is empty.
- Its name is the name of a registered tool.
- Its schema is not a JSON object.

The request lists the response tool after the registered tools, and it requires
a tool call. For Anthropic, the request sends `"tool_choice":{"type":"any"}`.
For OpenAI-compatible servers, it sends `"tool_choice":"required"`. A request
without a response format is byte for byte the same as before Scry had typed
turns. The model can still call registered tools for the number of rounds that
`max_tool_rounds` allows.

Scry classifies each response of a typed turn by its tool calls:

- A response that calls no tool fails the turn with `protocol`. The error names
  the response tool. It also tells if the server ignored the tool choice or if
  the output got to its token limit.
- A lone response-tool call is a candidate answer. Scry validates it inside
  `update()` on the host thread, as one delivery unit. If the validator accepts
  the answer, the turn completes. If the validator rejects it, the model gets
  `{"error": model_message}` as the tool-error result of that call. The
  rejection uses one tool round. For a reflected type, the text comes from the
  schema of the decoder, such as `$.supported is a required member`.
- A response-tool call can be beside other calls, or there can be more than
  one. Then Scry refuses it with `call respond exactly once, on its own, after
  your other tool calls have returned`. The real calls dispatch as usual.

Only real calls make an ordinary round. A server that forces a tool call can
report a plain `stop`. Thus a typed turn accepts calls with a normal finish
reason as a tool response.

Scry handles a validator as it handles a tool handler. If the validator returns
an error without a `model_message`, or if it throws, the model gets the fixed
text of the handler. The turn then continues. Without a validator, Scry accepts
any JSON object. Scry does not implement general JSON Schema validation.

The validator runs under the invocation guard of the handler. If the validator
cancels the turn, Scry obeys the cancellation before any verdict gets to the
worker. A disconnect from the validator stops later deliveries, but it does not
stop the verdict.

If Scry accepts an answer, the turn completes with `finish_reason` `completed`.
`Completion::structured` holds the canonical arguments. `Completion::text`
holds only the prose beside the call, and this prose can be empty. The committed
transcript has these parts:

- The user message.
- Each round of real calls, with its results.
- A final assistant message. It has that prose, followed by one text block that
  holds the canonical JSON of the answer.

Scry commits no response-tool call. Scry also does not commit a rejected attempt
that the model saw in the turn. Scry removes those calls, their error results,
and any message that they leave empty. Real calls in the same rounds stay. There
are three reasons for this:

- Each committed call needs its result.
- A result for the accepted call ends the history on a user message.
- Later requests do not declare the response tool, and a provider can refuse a
  history that names an undeclared tool. These later requests include plain
  turns and Conversations restored from JSON.

When Scry removes a rejected round, two assistant messages can be adjacent. The
Anthropic adapter merges them, and the OpenAI-compatible dialect accepts them.
The OpenAI adapter joins the text blocks of a message without a separator.
`Conversation::to_json()` saves the answer as the text block that it is. A
restored Conversation encodes again for either dialect. Scry reserves the answer
against the Conversation byte limit, as for any reply. Scry never charges the
`structured` copy to the queued-event limit.

Response-tool calls are part of the Scry protocol. They are not host tool calls.
They never get to a handler, `max_tool_calls_per_turn`, `on_tool_request`, or
`on_tool_call`. They are not in `tool_call_count` and not in
`rejected_tool_call_count`. `Completion::answer_attempt_count` counts each of
them, the accepted one included.

The `ToolCall::index` of a real call keeps its position in provider order. Thus
a response-tool call beside it still uses an index. `tool_round_count` includes
rounds whose only call was a rejected answer. The response that carries the
accepted answer is the final response, not a round.

Scry validates a lone response-tool call even when all rounds are used, because
it can end the turn. At the round limit, any other response fails with
`max_tool_rounds`, and a rejected answer also fails. This is true under each
`ToolRoundLimitPolicy`, because there is no answer to complete with.
`Completion::unexecuted_tool_calls` is always empty for a typed turn.

In all other respects, a typed turn fails, cancels, retries, and persists as
any turn does:

- Retries apply before semantic output.
- A failed or cancelled turn commits nothing.
- `on_finished` is the single terminal channel.

`ask()` reports what `send_and_wait_structured()` reports. It can also report a
decode failure of an accepted answer, but the validator makes this failure
unreachable.

## Providers and transport

The public `Message` model has user and assistant roles with text blocks,
tool-call blocks, and tool-result blocks. Provider adapters translate this model
into HTTP requests and decode the streamed replies.

A retry or a tool round sends the committed and pending messages again. Scry
writes each stored payload into the request as the canonical text that it
already holds, and it rejects malformed embedded text with `invalid_config`.

Stream decoding ignores the members of a provider event that Scry does not
read, because providers add fields. A payload whose shape is incorrect is a
`protocol` error that names the JSON path. The reads of the provider's error
token and request identifier are best-effort. Each value is independently
optional. A value of the wrong type reads as absent, and the event does not
fail.

| Setting | Anthropic Messages | OpenAI-compatible Chat Completions |
|---|---|---|
| Endpoint | `/v1/messages` | `/v1/chat/completions` |
| Authentication | Required `x-api-key` | Optional bearer token |
| `temperature` | 0–1 | 0–2 |
| `top_p` | Greater than 0, at most 1 | 0–1 |
| `max_tokens` | Required, positive | Optional; positive when set |
| `seed` | Rejected during validation | Optional; sent when set |
| Disabled reasoning | Rejected during validation | Sends `reasoning_effort: "none"` |

Scry passes `SamplingConfig::seed` through, but it does not enforce it. Scry
sends the same value with every request, including retries and tool rounds, and
it does nothing more. The server decides if the same seed, model, prompt, and
sampling values repeat an output. The server treats this as best-effort. No seed
carries across models, servers, or server versions.

The Messages API has no seed. Thus the Anthropic dialect rejects a seed and does
not drop it. If the dialect dropped the seed, a host can think that its runs
used a seed.

For Anthropic, use an origin or the full `/v1/messages` endpoint. The OpenAI
adapter accepts an origin, a `/v1` base, or the full `/v1/chat/completions`
endpoint. Both adapters always request `stream: true`. The default reasoning
mode omits the reasoning controls.

An OpenAI-compatible server must implement the subset that Scry sends. This
subset includes the optional reasoning field when it is enabled. It also
includes `tool_choice: "required"` for a typed turn. Scry does not implement
Azure-specific endpoints, the Responses API, server-side structured output modes
such as `response_format`, or other server extensions.
[Typed completions](#typed-completions) use tool calling instead, which both
dialects have.

The Anthropic adapter merges consecutive messages with the same role into one
message. The content array of this message concatenates their blocks. The
adapter does this because the Messages API takes one message per role turn.
Also, a history that stopped at the tool-round limit can end with the user
message that carries the results of that round.

OpenAI requests encode system text and function tools. They emit a separate,
ordered `role: "tool"` message for each result. Thus a `user` message can follow
tool results directly, and no merge is necessary. The stream decoder accumulates
bounded tool-call fragments by index. At finish, it requires complete contiguous
calls. A finish reason followed by `[DONE]` completes the stream.

After the finish reason and before `[DONE]`, the stream can have a chunk that
has only usage. These
conditions are protocol errors: missing, duplicate, or early terminal markers,
and semantic content after finish. For Anthropic streams, Scry decodes Messages
content blocks, usage, stop reasons, and tool-use arguments. Scry ignores
thinking and redacted thinking blocks. A compatible server can send these blocks
without a request. Scry does not keep their text, and it does not send them
back.

The incremental SSE parser handles byte splits at any position. A CR, LF, or
CRLF ends a line immediately when it arrives. If a lone CR ends a blank line,
the parser dispatches the event and does not wait for the next byte. The parser
can ignore unknown optional events. Malformed required content fails with
`protocol`. Scry has no non-streaming response path and no public logging API.

The transport uses libcurl through an internal injectable interface. Each
Harness keeps a curl multi handle and its connection cache across retries, tool
rounds, and turns. It runs one transfer at a time. Curl objects use RAII, and C
callbacks catch exceptions. Scry attempts the process-wide curl initialization
once and caches its result. At startup, Scry requires libcurl 7.84 or newer with
thread-safe global initialization and asynchronous DNS.

TLS peer verification and hostname verification are enabled by default.
`ca_bundle_path` selects a CA bundle, and `proxy` selects a proxy. Empty values
keep the curl defaults for the trust store and the proxy, including proxy
environment variables. Scry validates extra header names and values, and it
rejects collisions with headers that Scry manages. `Harness::validate()` checks
configuration values. It does not check if the server is reachable or if the
server accepts the credentials.

The SSE decoder does not receive non-2xx bodies. Scry keeps a maximum of 8 KiB
of such a body. From this text, Scry extracts a sanitized `error.type` or
`error.code` token into `provider_detail`, with the dialect name as a prefix.
Scry does not show the message of the provider or the raw body. When they are
available, HTTP errors carry their status and the sanitized request identifier.
Scry uses fixed diagnostics, and it removes matches of the configured API key
from the worker error fields and the request-ID fields.

## Resource limits and timeouts

`include/scry/config.hpp` defines the defaults:

| Setting | Default |
|---|---:|
| Pending turns per Harness | 64 |
| SSE event bytes | 256 KiB |
| Response bytes per transfer | 8 MiB |
| Tool argument bytes per call | 1 MiB |
| Tool result bytes per call | 4 MiB |
| Queued event payload per turn | 2 MiB |
| Conversation payload | 16 MiB |
| Tool rounds | 8 |
| Tool calls per turn | Unset |
| Maximum output tokens | 1024 |
| Retry attempts / elapsed window per model request | 3 / 30 s |
| Initial / maximum retry backoff | 250 ms / 10 s |
| Retry jitter ratio | 0.2 |
| Connect / idle / shutdown | 10 s / 120 s / 2 s |
| Total transfer timeout | Unset |

Resource limits must be positive. The queued-event limit must be at least 1024
bytes. `max_tool_calls_per_turn` is optional. If it is unset, the number of
calls is unlimited. Scry rejects a value of zero with `invalid_config`. An
admission failure rejects `send()` immediately. If an accepted turn exceeds its
limit, the turn fails with `resource_limit`.

The Conversation accounting includes the system prompt, text, tool identifiers
and names, tool-result error flags, and serialized arguments and results. It
includes these items across the history and the pending exchange. It excludes
JSON envelope syntax and allocator overhead, so it is not the size of
`to_json()`. The pump and the machine reserve exchange payloads before resend
and commit.

When the machine reserves a completion payload, Scry charges it to the
Conversation budget, never to the queued-event limit. Thus a completion that
fits the Conversation limit is always deliverable. The queued-event limit
applies to the text deltas, tool-call batches, and error diagnostics that wait
for delivery. `from_json()` has no Harness configuration and does not apply
these byte limits. These limits apply when the host sends the restored
Conversation through a Harness.

`connect` limits the connection establishment, including name resolution.
`idle` uses the curl low-speed check at one byte per second, and Scry rounds the
duration up to whole seconds. `idle` finds long silence, including before the
first byte. But it uses a rolling average, and it can report a stall later than
the configured interval. It is not an exact timer between chunks.

The optional `transfer` setting sets a hard duration limit on one HTTP transfer.
Timeout failures are retryable network errors, subject to the retry eligibility
of the turn. `shutdown` limits curl poll waits.

## Completion, errors, and history

Before acceptance, errors return directly. After acceptance, `on_finished` is
the single channel for the terminal result: completion, failure, or cancellation.
The host must continue to call `update()`. Scry delivers a callback once, unless
the host disconnects it or Harness destruction discards it. Allocation failures
are outside the semantic failure-as-value contract.

| Error category | Meaning |
|---|---|
| `invalid_config` | Configuration or serialized document is not valid |
| `invalid_state` | Operation is not valid for the current object state |
| `invalid_argument` | Caller argument is not valid, including a duplicate tool name |
| `busy` | Conversation already has an accepted turn |
| `authentication` | Provider authentication failure |
| `rate_limit` | Provider rate limit |
| `network` | Network or transport failure, including HTTP 5xx |
| `protocol` | Provider output is not valid |
| `resource_limit` | Admission bound or payload bound exceeded |
| `tool` | Tool arguments, dispatch, or encoding are not valid |
| `max_tool_rounds` | Tool-round limit exceeded |
| `cancelled` | Cooperative cancellation |

If terminal processing in `update()` succeeds, it commits the user message, the
tool rounds, and the final assistant response together. This commit occurs
before the delivery of the terminal callback. These messages arrive as one
transcript. The machine keeps a single message list, sends it again in each
round, and gives that same list to the pump. Failure or cancellation commits
nothing.

`Completion::finish_reason` is `completed`, `length`, `unknown`, or
`tool_round_limit`. `tool_use` is internal to the loop. A response that requests
tools starts another round, or fails with `max_tool_rounds`, or, under
`ToolRoundLimitPolicy::complete`, ends the turn as `tool_round_limit`. If the
application requires an answer that is not truncated, examine
`Completion::finish_reason`. A turn that has a response format completes only
on an accepted answer. It completes with `completed`, and
`Completion::structured` is engaged. [Typed completions](#typed-completions)
describes its transcript and its counts.

`Completion::unexecuted_tool_calls` holds the tool calls that the final response
asked for and that the loop never dispatched. These calls are in provider order.
This list has items only for `tool_round_limit`. During the turn, Scry reserves
these calls against the Conversation byte limit, exactly as for the tool round
that they replace. Scry never charges them to the queued-event limit. Scry gives
them to the host and does not commit them to history.

These calls are not in committed history, and their handlers never ran. Thus
they are not in `tool_call_count` and not in `rejected_tool_call_count`: the
model asked and got no answer.

`Completion::tool_round_count` and `Completion::tool_call_count` report what the
loop ran before that final response. The call count includes unknown tools and
calls whose handler failed. It excludes calls to the response tool of a typed
turn, which `Completion::answer_attempt_count` counts.
`Completion::rejected_tool_call_count` is the subset
of those calls that never got to a handler, because the per-turn call limit or
`on_tool_request` refused them. A refusal is an answer to the model, not a turn
failure, so it is in both counts. Each observed `ToolCall` carries its own
`round` and its `index` in the batch of that round, in provider order.

Text deltas can include intermediate tool rounds. `Completion::text` contains
only the final assistant response. But Scry delivers the deltas of round N+1
only after every `on_tool_call` of round N. Thus a host can count the
`on_tool_call` observations to find the round of each delta.

Each committed message has at least one block and no empty text block. The
machine removes empty text blocks from a model response before it commits or
dispatches anything. Thus, if a response has one empty text block and real text,
only the text commits. If a response announces a tool call together with an
empty text block, only the call commits. If a response has no text and no tool
calls after this removal, the turn fails with `protocol` and commits nothing. As
a result, `to_json()` can always encode committed history, and committed history
never has the empty content that providers reject on resend.

`Conversation::messages()` exposes committed history, without the system prompt.
Its reference is borrowed until an `update()` that commits, or until the handle
is moved or destroyed. Callback views and references are borrowed only for the
invocation. `on_finished` receives its result by value.

`to_json()` writes the system prompt and the committed message blocks as a
canonical versioned document. `from_json()` rejects these inputs with
`invalid_config`: malformed JSON, unknown fields or versions, and invalid block
shapes or roles.

The document is a reflected type that holds `messages`, `system_prompt`, and
`version`. Scrydantic decodes it after Scry reads only the `version`. Thus
`from_json()` reports another version as unsupported, and not as a shape
failure. A shape failure gives its JSON path, as in `Conversation document at
$.messages[0].role is a required member`. After the codec, Scry checks the
rules that the codec does not express:

- Each member of each message and block is present.
- Tool calls are only in assistant messages, and tool results are only in user
  messages.
- Text, identifiers, names, and content are not empty.
- Arguments are a JSON object.

A save while the Conversation is busy captures the last
committed boundary. The save excludes active work, callbacks, turn IDs, and
tools. Scry does no file I/O for persistence. The host owns storage and any
input-size limit before it loads a document.

## Build and package

The consumer target is `scry::scry`. It is a static library, and it requires GCC
16.2 or newer with C++26 reflection and annotation support. The supported
platforms are Linux and macOS. Before 1.0, Scry does not promise API, ABI, or
persistence-format stability. The package version check accepts only the same
minor version: `find_package(scry 0.7)` does not accept a 0.8 install.

Scry depends on the external Scrydantic package, version 0.2.x. Scrydantic has
two targets. `scrydantic::json` is a static C++23 library without reflection:
the JSON core, `Json`, `JsonView`, and `scrydantic::Error`.
`scrydantic::scrydantic` is header-only C++26 on top of it: the annotations,
the codec, the schemas, and the tool binding. `scry::scry` links
`scrydantic::scrydantic` PUBLIC, and the installed `scry` package finds
`scrydantic` as a dependency. Thus a program that links `scry::scry` can also
use Scrydantic directly.

CMake reuses Scrydantic targets supplied by a parent project, prefers a
compatible installed package, and otherwise fetches a pinned upstream revision.
When CMake fetched Scrydantic, `cmake --install` installs it beside Scry. The
linked dependencies are Scrydantic's JSON core and libcurl. The installed
package finds Scrydantic, curl, and Threads.

Public headers use Scry types, Scrydantic's JSON and error types, and move-only
`UniqueFunction` callables. Stateful handles use PImpl.
[development.md](development.md) describes the source layout and how to build
and test Scry itself.

### The testing component

`scry::testing` is an optional second static library. It is installed as the
package component `testing`, and it is built unless
`SCRY_BUILD_TESTING_SUPPORT` is off. It links only `scry::scry` and uses only
the public headers. It publishes `scry::testing::ScriptedServer`, an HTTP/1.1
server on 127.0.0.1 with an ephemeral port. The server answers each request with
the next response of a script, and it records each request.

A test sets `Config::base_url` to the URL of the server and calls
`Harness::create`. Thus a scripted turn uses all of the shipping code: libcurl,
the HTTP response policy, the SSE parser, the provider request encoder and
stream decoder, the retry schedule, the tool dispatch, and the pump. Its
guarantees are the guarantees that this document gives above.

A scripted response has a status, headers, and body chunks. The server sends
each chunk as one HTTP chunk. Each response has `Connection: close`, and the
server closes the connection after the response. A response can hold before
its first byte, or pause after a number of chunks, until the test releases it.
A response can also close the connection after a number of chunks. libcurl then
reports a retryable `network` failure. A request that arrives when the script
is empty gets status 404. Scry reports it as a `protocol` failure.

Retry waits are real time, and the retry policy of the `Config` sets their
bound. The server does not test TLS. The headers of `scry::testing` depend only
on `<scry/*>`.
