# Scry

> *Scrying: the practice of consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. Scry's stable
C++23 surface turns explicit schemas and callables into LLM tools and hides the
full agentic loop — HTTP, streaming, tool dispatch, retries — behind a small,
poll-friendly API. M3 adds isolated **C++26 reflection** to derive the same
registrations from ordinary C++ types; its contract, implementation, package
boundary, sanitizer leg, and runtime coverage gate are complete under ADR
0007.

The name is the design: **reflection** (the mirror) + **consulting an oracle** (the LLM). Namespace `scry::`, suggested repo name `scry` (fallbacks: `scry-cpp`, `scrylib`).

---

## 1. Vision

Python has a dozen mature LLM harnesses. C++ has llama.cpp bindings for local inference and almost nothing for API-based integration — yet the applications that live in C++ (games, CAD, trading systems, embedded, desktop tools) are exactly the ones that can't easily shell out to Python.

Scry lets an existing C++ application add LLM capabilities — chat *and* tool use — by touching roughly five types, with zero changes to its threading or event architecture.

**Guiding principle:** tool use is the core design target, not an add-on. Chat is the degenerate case of an agentic loop with zero tools. The architecture is built around the loop from day one.

## 2. Goals and Non-Goals

**Goals**

- Drop-in integration for apps with their own main loop (game engines, GUI apps, simulation loops). No event loop assumed, none imposed.
- M3 tool registration with near-zero boilerplate via C++26 reflection (P2996):
  schema generation and argument marshalling derived from plain structs at
  compile time, lowering to the implemented C++23 registry.
- The harness owns the agentic loop entirely: model requests tool → harness executes it → result appended → resend → repeat until final answer.
- Provider abstraction at the message level, not the HTTP level: Anthropic and
  the ADR 0008 OpenAI-compatible common subset for vLLM, Ollama, llama.cpp
  server, and LM Studio are implemented behind a config-only switch.
- Server/model configuration (base URL, auth, model, sampling params) as simple declarative config.
- Streaming, cancellation, and retries handled internally with clear thread guarantees.
- M5 examples that prove the public C++23 surface embeds in a real immediate-mode
  GUI and a small stateful game loop without expanding Scry's API or lifecycle.

**Non-Goals**

- Not an inference engine. Scry talks to servers; it does not load weights.
- Not a framework. Scry never owns `main()`, never spins an event loop the app must join, never demands ownership of app lifecycle.
- Not a GUI or game engine. Showcase views and world objects remain
  example-local; applications keep their window, rendering, input, state, and
  update-loop ownership.
- No prompt-template/chain DSL (LangChain-style). Apps compose in C++.
- MSVC support is deferred (no public P2996 support as of mid-2026 — see §9).

## 3. Target Environment

Primary: applications with a main loop that ticks at some frequency — game engines, Qt/ImGui/native GUI apps, simulators. Consequences that drive the whole design:

- **Never block by default.** Network turns take seconds; the main loop runs at 60 Hz or handles UI events. The async surface never waits on network I/O; the explicitly named `send_and_wait` convenience is reserved for CLI tools and tests.
- **Poll, don't push.** The app calls `scry::Harness::update()` once per tick. All callbacks fire inside `update()`, on the caller's thread. User code needs no locks.
- **Cancellation is normal.** Windows close, scenes change mid-request. Every in-flight turn has a `cancel()` safe to call at any time.

## 4. Core Concepts (the public surface)

The app touches five core concepts:

| Type | Responsibility |
|------|---------------|
| `scry::Config` | Plain value aggregate: base URL, API key, model, sampling params, provider dialect. Designated-initializer friendly. |
| `scry::Conversation` | Owns message history (system prompt, user/assistant turns, tool calls/results). Serializable for persistence. |
| `scry::ToolRegistry` | Named tools: description + schema + callable. Owned by a Harness and snapshotted when a turn is accepted. |
| `scry::Turn` | Handle to one in-flight agentic exchange. Carries callbacks (`on_text_delta`, `on_tool_call`, `on_complete`, `on_error`) and `cancel()`. |
| `scry::Harness` | Created from `Config`; owns provider/auth state, the tool registry, worker thread, and event queue. `send()` starts a turn; `update()` pumps completions into the app thread. |

Intended feel:

```cpp
// Config is a plain aggregate; Harness is the single configured runtime owner.
// Semantic failures are values. Allocation failure remains an ordinary C++ exception.
auto harness = scry::Harness::create(scry::Config{
    .base_url = "https://api.anthropic.com",
    .api_key  = env("API_KEY"),
    .model    = "claude-sonnet-5",
});
if (!harness) { /* invalid configuration, reported as a value */ }

auto conversation = scry::Conversation::create({
    .system_prompt = "Answer briefly and use tools when useful.",
});
if (!conversation) { /* invalid conversation configuration */ }

auto registered = harness->tools().add(
    scry::ToolDefinition{
        .name = "get_application_status",
        .description = "Report whether the host main loop is running",
        .input_schema = {
            .text = R"({"type":"object","properties":{},"additionalProperties":false})",
        },
    },
    [&](scry::Json arguments) -> scry::Result<scry::Json> {
        return app.status(arguments); // validate JSON; return valid JSON
    });
if (!registered) { /* invalid schema, duplicate name, or inactive registry */ }

auto turn = harness->send(*conversation, "Is the host application still running?");
if (!turn) { /* busy, invalid state, or admission/resource limit */ }
turn->on_complete([&](const scry::Completion& result) {
    ui.show(result.text);
});

// somewhere in the existing main loop:
while (app.running()) {
    harness->update();  // callbacks fire here, on this thread
    app.tick();
}
```

This explicit-schema overload is the implemented C++23 surface. The
[checked-in canonical example](examples/main_loop.cpp) registers a read-only
tool through it and drives the complete M2 loop. M3 adds
`scry::reflection::add<Args>()` schema generation and marshalling as
compile-time sugar over the same registry; it does not introduce a second
dispatch system.

**Conversation persistence.** `Conversation::to_json()` returns a canonical,
versioned Scry-owned JSON document suitable for app-managed storage;
`Conversation::from_json()` validates and restores one. Version 1 preserves
the system prompt and every committed neutral text, tool-call, and tool-result
block. Busy state and uncommitted turns are deliberately excluded, so saving
an active Conversation captures its last committed boundary. Scry does no file
I/O and rejects malformed documents, unknown fields or versions, and invalid
role/content combinations with `invalid_config`.

## 5. Architecture Overview

```mermaid
graph LR
    subgraph App["Host application (main loop)"]
        MAIN["App code<br/>tick / update()"]
        TOOLS["Registered tools<br/>(app functions)"]
    end

    subgraph Scry["Scry harness"]
        API["Public API<br/>Harness · Conversation · Turn"]
        REG["ToolRegistry<br/>(explicit schemas;<br/>P2996 lowering in M3)"]
        LOOP["Agentic loop engine"]
        Q["Event queue<br/>(worker → main)"]
        PROV["Provider adapters<br/>Anthropic · OpenAI-compatible"]
        NET["Worker thread<br/>HTTP + SSE streaming"]
    end

    LLM["LLM server<br/>(API or local: vLLM, Ollama,<br/>llama.cpp server)"]

    MAIN --> API --> LOOP
    TOOLS --> REG --> LOOP
    LOOP --> PROV --> NET <--> LLM
    NET --> Q --> MAIN
```

Layering rule: the app only sees the public API; the provider adapter only sees neutral `Message`/`ToolCall` types; only the adapter knows wire formats.

## 6. Interaction Model: the Agentic Loop

The harness owns the loop. The app registers tools and receives a final answer; intermediate tool calls are visible through optional callbacks but require no app participation.

```mermaid
sequenceDiagram
    participant App as App (main loop)
    participant H as Scry harness
    participant LLM as LLM server

    App->>H: send(convo, "user message")
    Note over App: keeps ticking, calls update()
    H->>LLM: POST /messages (history + tool schemas)
    LLM-->>H: stream: text deltas
    H-->>App: on_text_delta (via update())
    LLM-->>H: stop_reason: tool_use
    H->>H: execute registered tool<br/>(main thread by default, §7)
    H-->>App: on_tool_call (informational,<br/>after result is posted)
    H->>LLM: POST /messages (+ tool result)
    LLM-->>H: stream: final answer
    H-->>App: on_complete (via update())
```

The loop iterates as many times as the model requests tools, bounded by a configurable `max_tool_rounds` to prevent runaways.

## 7. Threading Model

One worker thread per `Harness` does all blocking I/O. A lock-free (or mutex-guarded, initially) event queue carries results back. **Every user callback fires inside `update()`, on the thread that calls it.** That is the contract that makes user code lock-free.

```mermaid
graph TB
    subgraph MT["Main thread (app-owned)"]
        U["harness.update()"]
        CB["user callbacks<br/>on_text_delta / on_complete / tool handlers"]
        S["send() / cancel()"]
    end
    subgraph WT["Worker thread (scry-owned)"]
        HTTP["HTTP + SSE parsing"]
        AL["agentic loop state machine"]
    end
    EQ["event queue"]
    CQ["command queue"]

    S --> CQ --> AL
    AL --> HTTP
    HTTP --> AL
    AL --> EQ --> U --> CB
```

**Tool execution policy.** App-thread execution inside `update()` remains the
default: it is the safe mode for handlers that touch host state. M4 implements
ADR 0009's `ToolExecution::app_thread` / `ToolExecution::worker_thread` through
`ToolRegistrationOptions`; execution policy stays separate from
provider-visible `ToolDefinition` metadata, and reflected registration
forwards the same option.

Worker mode does not create a pool or a second agent loop. An app-thread
handler remains in the pump-side accepted-turn snapshot. A
worker-thread handler moves once, through FIFO registration, into a
worker-owned table; subsequent accepted turns cross the boundary with neutral
schemas, execution modes, and worker tool names, never a shared callable. The
complete provider batch is still admitted atomically. `update()` dispatches
calls in provider order, posts an opted-in call to the worker, and pauses later
calls until the worker applies the canonical result to the same machine and
the pump receives an accepted-result acknowledgement. Tool observers still run
only in `update()` after that acceptance.

**Frame budget.** `update()` accepts an optional time budget; excess events roll to the next tick. The budget is a soft deadline checked between callbacks. Scry never preempts user code, so one slow callback or tool handler can overrun it.

**Cancellation.** `Turn::cancel()` sets an atomic flag; the worker aborts the HTTP transfer at the next opportunity and posts a `Cancelled` event. Cancelling a still-queued turn removes it before any I/O is issued. `Turn` handles are safe to drop (detach semantics); dropping does not join, block, or cancel.

Tool handlers are non-preemptive in either mode. Cancellation observed before
dispatch skips the handler. Cancellation requested while one runs takes effect
when it returns: Scry suppresses that result and the remaining calls, does not
resend to the model, and terminates the turn as cancelled. Scry can bound its
own transport shutdown, but cannot safely terminate arbitrary C++ user code.
Opting into worker execution therefore requires the application handler to
return within the application's teardown bound; the configured Scry-owned
shutdown bound excludes time spent inside that handler.

**Batch and payload atomicity.** All tool calls from one assistant response are
admitted to the worker-to-pump event queue as one batch. If the whole batch
cannot fit, none becomes dispatchable and no handler runs. During pump
dispatch, a fatal framework failure (for example, no remaining space for even
a bounded result) suppresses every later handler in that batch. The
Conversation byte limit is one cumulative exchange budget: assistant tool-call
messages, every tool result, and the final answer are reserved before
dispatch, resend, or commit. It is not a per-message limit.

**M2 scheduling baseline (ratified).** A Harness accepts up to `Config::limits.max_pending_turns`; accepted turns queue FIFO and exactly **one HTTP transfer is active at a time**. Admission beyond that bound fails immediately with `resource_limit`. A second `send()` on a Conversation that already has a turn queued or in flight fails immediately with `busy`. While the active turn awaits a main-thread tool result, it retains the serialized turn slot, so queued turns wait (deliberate simplification; trigger and end state in the ARCHITECTURE.md evolution register, which moves to curl-multi multiplexing when serialized scheduling measurably limits a real app).

**Registry ownership and snapshots.** A Harness owns exactly one `ToolRegistry`;
there is no Conversation-local or process-global registry. `send()` snapshots
immutable registrations into the accepted turn, so later or reentrant
registration remains safe and affects only subsequently accepted turns. The
public surface cannot move the Harness-owned registry out. In the M4
implementation, definitions and execution modes are snapshotted; app handlers
remain pump-owned while worker handlers remain in the worker table installed
before the send by FIFO command order. Explicit schemas are parsed when
registered, must be JSON objects, and are stored canonically.

**Runtime configuration defaults.** Limits count payload bytes (not allocator
overhead); implementations may reject earlier when a provider's own limit is
lower. These defaults are conservative starting points and remain configurable:

| Setting | Default |
|---|---:|
| Pending turns per Harness | 64 |
| SSE event | 256 KiB |
| Response | 8 MiB |
| Tool arguments | 1 MiB |
| Tool result | 4 MiB |
| Queued event payload per turn | 2 MiB |
| Conversation payload | 16 MiB |
| Tool rounds | 8 |
| Default maximum output tokens | 1024 |
| Retry attempts / elapsed time | 3 / 30 s |
| Retry initial / maximum backoff | 250 ms / 10 s |
| Connect / transfer / shutdown timeout | 10 s / 120 s / 2 s |

TLS peer verification defaults on. The runtime uses Curl with asynchronous DNS,
applies Curl's connect timeout (which covers name resolution and connection)
and total transfer timeout, and caps each multi-poll wait by the shutdown
timeout. A runtime that cannot provide the required resolver/global
capabilities is rejected. Deterministic tests cover held transfers, cancellation,
and capability rejection; the timeout wiring is source-reviewed rather than
tested against a flaky DNS black hole.

## 8. Tool Registration: C++23 Now, Reflection in M3

The implemented M2 boundary accepts a `ToolDefinition` (name, description, and
JSON-object input schema) plus a move-only `Json → Result<Json>` handler.
Registration parses and canonicalizes the schema immediately. Each accepted
turn owns an immutable snapshot, sends its schemas with every model request,
dispatches calls on the `update()` thread, and automatically resends ordered
results. The handler is responsible for validating its explicit JSON arguments
and returning valid JSON; Scry converts unknown tools, handler errors,
exceptions, and malformed handler output into bounded model-visible tool
errors.

P2996 is the M3 ergonomics layer over that boundary. [ADR
0007](docs/adr/0007-m3-reflection-contract.md) fixes its contract. Its accepted
public shape is a free function in the optional component, keeping
experimental declarations out of the stable `ToolRegistry` class:

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
closed inline objects, and enum values preserve declaration order. The M3
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
duplicate lexical keys have already been collapsed by canonical parsing and
are not separately observable to the reflected decoder.

Parameter descriptions use Scry's P3394 `description` annotation when
supported. `tool_traits<Args>::descriptions` is the portable path and explicit
override; a matching trait entry wins while an absent entry falls back to the
annotation. Unknown/duplicate trait names and duplicate Scry description
annotations are compile-time errors.

Handlers receive a moved `Args` and may return a supported value directly or
inside `scry::Result`. Raw `Json`, `void`, `Status`, references, futures, and
awaitables deliberately stay outside the reflected overload; the implemented
explicit-schema API remains the escape hatch for dynamic or unsupported
boundaries.

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

**Glaze** is the ratified internal JSON dependency (ARCHITECTURE.md §9). The
reflection header and installed target expose only Scry-owned and standard
types; compiled implementation reaches Glaze through a Scry-owned JSON bridge.

The live M3 verification path runs 27 reflection-labelled tests: 22 runtime,
schema, codec, bridge, and registration cases plus five stable-diagnostic
compile failures. `scripts/reflection-coverage.sh` pins GCC/gcov 16 and gcovr
8.6 and gates with stock gcovr thresholds
([ADR 0011](docs/adr/0011-absolute-quality-gates.md)): at least 85% source
decisions and 95% functions in the runtime codec, and at least 95% GCC/gcovr
CFG branches in the compiled JSON bridge. The codec decision floor
accommodates the one inline-justified GCC-generated switch on the
reflected-enum decoder that gcovr's decision analysis still counts; the
earlier bespoke exclusion validator is retired. Consteval paths stay covered
by the positive/negative compile matrix; no reflection property/fuzz or
manual Clang result is claimed.

## 9. Provider Abstraction

Neutral internal model: `Message { role, vector<ContentBlock> }` where `ContentBlock` is text, tool call, or tool result. Adapters translate to wire formats:

- **Anthropic Messages API** — content blocks, `tool_use`/`tool_result`.
- **OpenAI-compatible Chat Completions (M4, implemented)** — OpenAI plus the
  common vLLM, Ollama, llama.cpp server, and LM Studio subset from ADR 0008.

Adapter differences (schema envelope, streaming event shapes, stop reasons)
remain entirely inside the adapter. The M4 endpoint rule accepts an origin or
path, `/v1`, or the full `/v1/chat/completions` endpoint and normalizes it
without Azure inference. Authentication is optional for local servers; a
nonempty CR/LF-free key becomes one Bearer header. Requests send only model,
messages, finite `temperature` in `[0,2]`, optional `top_p`, positive legacy
`max_tokens`, `stream`, streaming `include_usage`, and function tools. System
text becomes a system message, assistant tool calls retain stable IDs, and
each neutral tool result expands into one ordered `role: "tool"` message.

Non-streaming responses require a `chat.completion` object with exactly one
choice at index zero. Text may be a string or null; function calls require IDs,
names, and object arguments. `prompt_tokens`/`completion_tokens` replace the
neutral usage totals. Finish reasons map `stop` to completed, `length` to
length, `tool_calls` to tool use, and `content_filter` or an unknown future
string to unknown. Streaming requires a stable nonempty chunk ID and accepts
one indexed choice or a usage-only chunk, accumulates interleaved tool
fragments by index, and requires a finish reason before the sole successful
terminal marker, `[DONE]`. Conflicting metadata, sparse calls, deprecated
`function_call`, nonempty structured refusals, malformed required content, or
illegal lifecycle transitions are protocol errors. Per-turn decode state is a
dialect-specific variant, so Anthropic and OpenAI-compatible Harnesses cannot
contaminate one another. Switching dialects is only a `Harness` config change.

**Toolchain and package reality (mid-2026):** P2996 is in C++26.
[GCC 16 provides P2996R13](https://gcc.gnu.org/projects/cxx-status.html) behind
`-std=c++26 -freflection` and is the supported M3 reflection toolchain.
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

- **Retries:** exponential backoff with jitter for 429/5xx/transport errors, honoring `Retry-After`, under configurable attempt and elapsed-time caps. Retry eligibility is strict: a request is retried only if **no semantic output has been consumed** (failure before the first content event). After partial output the turn fails with a retryable-flagged error and the app decides — automatic mid-stream resumption is later hardening, not M1. Within one turn, retry machinery never dispatches the same tool-call ID twice. A failed or cancelled turn commits no tool rounds, so resubmitting the user message is **not** automatically safe for side-effecting tools; applications must supply their own idempotency keys or reconciliation policy.
- **Errors:** immediate API rejection (`create`, `send`, callback/tool registration) returns `std::expected<..., scry::Error>`. Once a turn is accepted, asynchronous failure has one channel: `on_error(scry::Error)`. Categories include invalid configuration/state, busy, authentication, rate limit, network, protocol, resource limit, tool failure, maximum tool rounds, and cancellation. Tool-handler exceptions are caught and returned to the model as tool errors (the model can often recover), not thrown into the app. Exceptions thrown by app callbacks are different: they propagate synchronously out of `update()` after the event is counted delivered.
- **Streaming:** SSE parsed on the worker; text deltas batched per `update()` tick rather than per-token, so a fast stream doesn't flood the queue. Callback arguments are borrowed for the duration of the invocation; apps copy any data they retain.

## 11. Open Questions

Resolved and removed from this list: concurrency baseline (§7), JSON library
(Glaze — ARCHITECTURE.md §9), and HTTP library (libcurl direct —
ARCHITECTURE.md §7). Remaining:

1. **Structured output** — reflected structs also enable "answer as this type" (schema-constrained responses). Natural v2 feature; keep the door open in `Turn`.
2. **Coroutine sugar** — `co_await harness.send(...)` for apps with coroutine schedulers. Tracked in the evolution register; layered over the event queue later.

## 12. Roadmap

| Milestone | Scope |
|-----------|-------|
| **M0 — Skeleton (complete)** | Compile-only public header sketch + canonical example; target-based build/install/package layout; stable Linux + macOS core CI; GCC 16 reflection feasibility with Glaze; clang-p2996 as a non-gating experiment; libcurl SSE feasibility probe. No runtime loop. |
| **M1 — Chat (complete)** | Config + Conversation + Harness + Turn; worker actor + update() pump; minimal sans-I/O request/turn machine including retries; Anthropic adapter; blocking + streaming text. ToolRegistry validation/storage is inert infrastructure only. |
| **M2 — Tools (complete)** | Snapshot and serialize explicit-schema registrations; multi-round tool states in the sans-I/O machine; main-thread ordered dispatch and automatic resend; transactional tool history and versioned Conversation persistence. |
| **M3 — Reflection (complete)** | Optional GCC 16 `scry::reflection` component; P2996 lexical schema generation and strict typed marshalling; `scry::reflection::add<Args>()`; annotation/trait descriptions; package consumer and docs demo. |
| **M4 — Breadth (complete)** | ADR 0008 OpenAI-compatible Chat Completions subset and ADR 0009 ordered per-tool worker execution; retries/backoff polish, cancellation hardening, and their deterministic, fuzz, sanitizer, Curl, and scheduled bounded local-model gates. |
| **M5 — Showcase (complete)** | ADR 0010 examples: an opt-in Dear ImGui chat panel and a deterministic grid world where the LLM drives an NPC through explicit tools, with no new public/package surface. |

### M5 showcase contract

The showcase is an integration proof, not a new framework layer. Its ImGui
panel consumes only `scry::scry`; the host creates and outlives the Harness and
Conversation, calls `update()` in its own loop, and owns the ImGui context,
window, renderer, and platform backend. The panel demonstrates asynchronous
submit, streamed text, complete/error/cancel states, and an explicit Cancel
control. It may use an example-private controller to make those transitions
deterministic in tests. Destroying the panel requests cancellation but never
blocks.

The NPC example uses explicit closed-empty-object schemas for `look` and four
cardinal move tools. A fixed 5-by-5 in-memory grid makes observations,
successful movement, and blocked boundaries reproducible. Those handlers stay
on the app thread because they touch host state. The world is deliberately
ephemeral: it demonstrates the tool loop, not transactionality for game state.
Applications adapting the example to durable effects remain responsible for
idempotency or reconciliation across failed, cancelled, or resubmitted turns.

Dear ImGui is build-only showcase material, default OFF, pinned to `v1.92.8`
commit `8936b58fe26e8c3da834b8f60b06511d537b4c63` under its MIT license.
No platform or renderer backend is selected. No ImGui type, source, target, or
dependency may enter Scry's public headers, install, exports, or normal runtime
dependency set. SHOW-001–004 and ADR 0010 define the acceptance boundary; M5 is
not complete until the deterministic panel/NPC tests, real headless ImGui frame,
warnings-as-errors build, package audit, and shared local/hosted gate pass.
Those checks pass locally through `scripts/ci-showcase.sh` and the complete
preflight and in hosted CI through the same script. M5 is complete.

M1 and M2 precede M3 on purpose: reflection is the flashy layer, but it now
has a complete, tested C++23 agentic runtime to lower onto.
