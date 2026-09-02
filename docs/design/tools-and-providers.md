# Design: Tools and Providers

## 8. Tool Registration: Explicit Schemas, Optional Reflection

The stable boundary accepts a `ToolDefinition` (name, description, and
JSON-object input schema) plus a move-only `Json → Result<Json>` handler.
Registration parses and canonicalizes the schema immediately. Each accepted
turn owns an immutable snapshot, sends its schemas with every model request,
dispatches calls on the `update()` thread, and automatically resends ordered
results. The handler is responsible for validating its explicit JSON arguments
and returning valid JSON; Scry converts unknown tools, handler errors,
exceptions, and malformed handler output into bounded model-visible tool
errors. Handlers read their arguments with `scry::JsonView` — kind, `find()`,
`at()`, and ordered `key_at()` over the canonical document — rather than
comparing argument text, and build small results with
`scry::escape_json_string()`. The messages those calls and results become are
the public `scry::Message` family, so a host can render the same history the
next request will send.

P2996 is an optional, experimental ergonomics layer over that boundary. Its
contract is defined here and in the SCRY-TOOL requirements. Its public shape is
a free function in the optional component, keeping experimental declarations
out of the stable `ToolRegistry` class:

```cpp
struct ForecastArgs {
    [[=scry::reflection::description{"City to query"}]]
    std::string city;
    std::optional<int> days = std::nullopt;
};

struct Forecast {
    std::string summary;
    double temperature_c;
};

auto status = scry::reflection::add<ForecastArgs>(
    harness->tools(),
    {
        .name = "forecast",
        .description = "Return the forecast for one city",
    },
    [](ForecastArgs args) -> scry::Result<Forecast> {
        return lookup_forecast(std::move(args));
    });
```

`input_schema_v<Args>` is a compile-time, minified canonical JSON schema.
Objects and property names are sorted lexicographically, nested aggregates are
closed inline objects, and enum values preserve declaration order. The
value family is deliberately finite: booleans; non-character integral types;
finite `float`/`double`; owning strings; scoped enums; optionals; vectors
except every `vector<bool, Allocator>` specialization; fixed arrays; and
recursively supported plain aggregates. Scoped enum values must be unique, and
only one optional layer is supported: enum aliases and nested optionals are
rejected because their JSON representation cannot preserve the C++ distinction.
Integers carry their C++ range, fixed arrays carry their exact length, and
unsupported shapes fail at the registration call. Glaze supporting another
C++ type does not silently add it to Scry's contract.

**Presence and null are separate.** A default member initializer alone permits
omission; `std::optional` alone permits JSON `null`. Consequently
`std::optional<T> value;` is required-but-nullable, while
`T value = initializer;` is omittable-but-non-null. If an initialized member is
omitted, normal C++ construction preserves its initializer. Generated schemas
never publish a JSON Schema `default`.

The generated decoder rejects a non-object root, unknown fields, missing
required fields, wrong JSON kinds, disallowed null, numeric sign/range or
non-finite errors, unknown enum names, and fixed-array length mismatches at
every nesting level. These are bounded model-visible tool errors, not fatal
turn failures. The canonical parsed JSON value is the dispatch boundary:
duplicate lexical keys have already been collapsed by the single canonical
parse and are not separately observable to the reflected decoder.

Tool and parameter descriptions come from Scry's P3394 `description`
annotation and from nowhere else — there is no portable trait-based fallback or
override, so a description has exactly one source and lives next to the member
it describes. Duplicate description annotations on one entity are a
compile-time error.

Handlers receive a moved `Args` and may return a supported value directly or
inside `scry::Result`. Raw `Json`, `void`, `Status`, references, futures, and
awaitables deliberately stay outside the reflected overload; the implemented
explicit-schema API remains the escape hatch for dynamic or unsupported
boundaries.

The same closed value encoder is available directly as
`scry::reflection::encode(value)`. It returns Scry-owned canonical `Json`
without registering or invoking a tool, so an application can retain typed
values until a JSON boundary actually needs them. Direct and handler-result
encoding share one implementation and error mapping. The output is not a
versioned persistence document and carries no cross-version byte guarantee.

**Explicit-schema registration (not a parallel system):** the registry's internal representation is necessarily runtime data — name, description, schema JSON, type-erased `json → json` callable — since that is what gets serialized to the server and dispatched on tool calls. The reflection API is `consteval` sugar that lowers onto this same table, so exposing the lower layer as a public overload costs one function signature, not a second code path to maintain. It earns its keep twice: today it covers toolchains without P2996; permanently it covers *dynamic* tools whose schemas exist only at runtime (plugin-loaded tools, MCP proxying, user scripting) — something compile-time reflection can never express. If universal P2996 adoption arrives, the overload remains as the dynamic-tool API rather than becoming debt.

**Side effects and idempotency.** Scry guarantees at-most-once dispatch for a
tool-call ID within one accepted turn. It cannot make external effects
transactional: cancellation may arrive after a non-preemptive handler has
changed app state, and a failed/cancelled turn intentionally commits no
Conversation history. A host that lets tools charge a card, write a file, send
a command, or otherwise mutate durable state must therefore include an
app-owned operation/idempotency key in that tool's schema. Before applying the
effect, the handler checks a durable key→result ledger and returns the recorded
result for duplicates; after an ambiguous failure, the app reconciles that
ledger before resubmitting the user request. Read-only handlers, such as the
canonical `get_application_status` example, need no such policy.

**Glaze** is the ratified internal JSON dependency
([dependency architecture](../architecture/dependencies-and-errors.md)). The
reflection header and installed target expose only Scry-owned and standard
types; compiled implementation reaches Glaze through a Scry-owned JSON bridge.

## 9. Provider Abstraction

Neutral internal model: `Message { role, vector<ContentBlock> }` where `ContentBlock` is text, tool call, or tool result. Adapters translate to wire formats:

- **Anthropic Messages API** — content blocks, `tool_use`/`tool_result`.
- **OpenAI-compatible Chat Completions** — OpenAI plus the
  common vLLM, Ollama, llama.cpp server, and LM Studio subset.

Adapter differences (schema envelope, streaming event shapes, stop reasons)
remain entirely inside the adapter, and every adapter receives a `Config` that
`Harness::create` already validated. The OpenAI-compatible dialect deliberately
targets a *common subset*, not parity: it normalizes an origin, a `/v1` base,
or a full `/v1/chat/completions` endpoint without Azure inference; makes
authentication optional so local servers need no key; allows `max_tokens` to be
left unset, in which case the field is omitted from the request and the server
default applies; keeps the portable request baseline free of provider
extensions; and expands each neutral tool result into its own ordered
`role: "tool"` message. One explicit opt-in exists:
`ReasoningMode::disabled` sends `reasoning_effort: "none"` for endpoints that
support it, while the default omits the field. PROV-003/010/011 state the
binding form, including the strict streaming lifecycle:
bounded index-accumulated tool fragments, a required finish reason, and `[DONE]`
as the sole successful terminal marker, with anything else a protocol error.

The adapter seam is streaming-only: the runtime always requests
`stream: true` and decodes every response through the stream path, so there is
no parallel non-streaming decoder to keep in sync (see the
[evolution register](../architecture/quality-and-evolution.md)). Per-turn decode
state is a dialect-specific variant,
so Anthropic and OpenAI-compatible Harnesses cannot contaminate one another.
Switching dialects is only a `Harness` config change.

**Toolchain and package reality (mid-2026):** P2996 is in C++26.
[GCC 16 provides P2996R13](https://gcc.gnu.org/projects/cxx-status.html) behind
`-std=c++26 -freflection` and is the supported reflection toolchain.
Reflection is opt-in at build time and is consumed as
`find_package(scry CONFIG REQUIRED COMPONENTS reflection)` plus
`scry::reflection`; the core `scry::scry` package remains C++23 and does not
install or export reflection support when built with the feature off.
Bloomberg's [clang-p2996 fork](https://github.com/bloomberg/clang-p2996)
remains useful for manual compatibility experiments, but it is not a supported
reflection configuration and produces no installable or release artifacts.
Stable GCC/Clang build the severable C++23 core with reflection disabled. MSVC
reflection support remains deferred.

## 10. Errors, Retries, Streaming

- **Retries:** exponential backoff with independently seeded per-Harness jitter for 429/5xx/transport errors, honoring `Retry-After`, under configurable attempt and elapsed-time caps. Retry eligibility is strict: a request is retried only if **no semantic output has been consumed** (failure before the first content event). After partial output the turn fails with a retryable-flagged error and the app decides — automatic mid-stream resumption is later hardening. Within one turn, retry machinery never dispatches the same tool-call ID twice. A failed or cancelled turn commits no tool rounds, so resubmitting the user message is **not** automatically safe for side-effecting tools; applications must supply their own idempotency keys or reconciliation policy.
- **Errors:** immediate API rejection (`create`, `send`, tool registration) returns `std::expected<..., scry::Error>`. Once a turn is accepted, every asynchronous outcome uses one channel: when supplied, `on_finished(scry::Result<scry::Completion>)` carries the completion or the `scry::Error`. Categories include invalid configuration, argument, or state, busy, authentication, rate limit, network, protocol, resource limit, tool failure, maximum tool rounds, and cancellation. Tool-handler exceptions are caught and returned to the model as tool errors (the model can often recover), not thrown into the app. Exceptions thrown by app callbacks are different: they propagate synchronously out of `update()` after the event is counted delivered. A failure produced by a provider HTTP response carries that response's status in `http_status`, and where the response body names its own error type or code, a sanitized `provider_detail` token under the dialect namespace (`anthropic:not_found_error`); the provider's message and the rest of the body are discarded.
- **Streaming:** SSE parsed on the worker; text deltas batched per `update()` tick rather than per-token, so a fast stream doesn't flood the queue. The text-delta view and tool-call reference are borrowed for the invocation; apps copy data they retain. `on_finished` receives its result by value.
