# Scry — Architectural Patterns & Implementation Practices

Companion to [DESIGN.md](DESIGN.md). That document says *what* the pieces are; this one says *how* each piece is built — the C++ design patterns, idioms, and practices we commit to, and why. Deliberately no class listings or method signatures; those belong to the M0 header sketch.

---

## 1. Library-Wide Principles

These apply everywhere and settle arguments before they start.

**Value semantics at the boundary, ownership inside.** Public types are cheap-to-move value types or lightweight handles. Internally, every resource has exactly one owner (`std::unique_ptr`, RAII wrappers); `shared_ptr` appears only where lifetime is genuinely shared across threads — which after the §3 ownership split is exactly one node: the per-turn cancellation flag, jointly referenced by handle and worker. Raw pointers are non-owning observers only, never stored across a suspension point.

**Rule of Zero.** Types define no special member functions unless they manage a resource directly; resource management is pushed into dedicated RAII wrappers (curl handles, threads, queues) so everything above them defaults.

**PImpl on stateful handles; plain values for contracts.** The five stateful handle types (Client, Conversation, ToolRegistry, Turn, Harness) hold a single pointer to an implementation — ABI stays stable across internal refactors, and compile-time cost for consumers stays flat. Everything else the user touches is an ordinary value type: configuration aggregates (designated-initializer friendly), errors, options, enums, event payloads, and the Scry-owned JSON boundary type. The binding rule underneath both is: **no third-party types in public headers, ever** (enforced by include audit). Handles are constructed *from* config values (`Client` from a `Config` aggregate), which is how aggregate-style configuration and PImpl coexist. The indirection cost is irrelevant next to network latency — this library's hot path is measured in milliseconds, not nanoseconds.

**No singletons, no globals, no static init order problems.** Everything hangs off a `Harness` instance. Two harnesses in one process (e.g., different providers) must just work. The one unavoidable global — libcurl's `curl_global_init` — is wrapped in a reference-counted RAII guard (Meyers-style function-local static, initialized on first Harness).

**Exceptions stop at the membrane.** Internals may use whatever is idiomatic for the dependency at hand, but the public boundary is exception-free by contract: fallible operations report through `std::expected` or the `on_error` callback. Precisely scoped: *semantic* failures (bad config, duplicate tool name, invalid state) never throw — fallible construction goes through factories returning `std::expected`, and registration returns `expected` too. Allocation and standard-library construction failure (`std::bad_alloc`) are **excluded from the contract** — we do not pretend to survive OOM, and smearing `noexcept`+expected over every allocating call would buy nothing. Two hard rules stand regardless: nothing ever throws *across* the worker/main thread boundary, and tool-handler exceptions are caught at the dispatch site and converted into tool-error results returned to the model. User callbacks should not throw; if one does, the exception propagates out of `update()` to the app with the harness left in a valid state and the event counted as delivered (see §3).

**Concepts over inheritance in templates, interfaces only at seams.** Virtual dispatch appears in exactly two places (provider adapter, transport — §6, §7), both internal. The public API has no inheritable types; extension points are callables and config, not subclassing.

**C++ standard posture.** Core library targets C++23 (`std::expected`, `std::move_only_function`, deducing this where useful). The reflection layer is an isolated C++26 module of the codebase, kept severable (see §5).

## 2. The Concurrency Architecture: Actor, Not Locks

The single most important structural decision. The worker thread is an **actor**: it exclusively owns all mutable networking and loop state, and (bar the per-turn cancellation atomic, §3) the only way anything crosses the thread boundary is **message passing** through two queues — a command queue in (send, cancel, shutdown) and an event queue out (deltas, tool requests, completions, errors).

Practices that follow:

- **Enumerated shared state, no user-visible locks.** There is no mutex a user callback can deadlock against. Exactly three things are shared across the thread boundary, all internally synchronized: the command queue, the event queue, and one atomic cancellation flag per turn. Nothing else — worker-side state and pump-side state are exclusively owned (ownership table in §3), and the worker addresses turns only by immutable `TurnId`. This enumeration *is* the invariant TSan enforces; anything not on the list found crossing threads is a bug by definition.
- **Messages are immutable values.** Commands and events are `std::variant` of small structs, moved (never copied) through the queue. Variant + `std::visit` gives exhaustive handling — adding an event type breaks the build until every consumer handles it. This is the same closed-set-of-alternatives reasoning that picks variant over inheritance everywhere in this codebase.
- **Queue implementation: boring first.** Mutex + `std::deque` + condition variable, wrapped behind our own minimal interface so a lock-free MPSC queue can be swapped in *if profiling ever demands it*. Premature lock-free is how libraries acquire unfixable bugs.
- **Two cancellation mechanisms, deliberately separate.** The worker's `std::jthread` `stop_token` means one thing only: **Harness shutdown**. Per-turn cancellation is a distinct per-turn `std::atomic<bool>`, checked at every I/O boundary and plumbed into transport progress callbacks. Both are cooperative, never `pthread_cancel`-style. Conflating them was an early ambiguity: shutdown must abort *all* turns and join; cancelling one turn must not disturb its neighbors.
- **The pump is the contract.** `update()` drains the event queue and invokes callbacks on the calling thread, under an optional time budget (deadline-checked between events, excess rolls to the next tick). Because *all* callbacks fire there, user code is single-threaded by construction. This is the harness's most sacred invariant; everything else may change.
- **Backpressure by design.** Streaming deltas are coalesced worker-side (one aggregated text event per pump interval, not per token) so a fast stream cannot flood the queue or starve the frame budget.

**Blocking-mode escape hatch:** a synchronous `send`-and-wait exists for CLI tools and tests, implemented *on top of* the async machinery (pump-until-complete internally), never as a second code path.

## 3. Turn Ownership, Lifecycle, and the Handle Pattern

A `Turn` is a **handle**: a move-only value holding an immutable `TurnId` plus a reference to the turn's cancellation flag — *not* the turn state itself. Copying a handle to in-flight work invites double-cancel ambiguity, hence move-only. This is the `std::future`/`std::stop_source` school: small, thread-safe by narrowness, no behavior hidden in destructors beyond a documented detach.

### Ownership table (normative)

| State | Exclusive owner | Notes |
|---|---|---|
| Transport handles, curl state, wire buffers, SSE parser state | Worker | Never visible to any other thread |
| Loop state machines (per turn) | Worker | Addressed by `TurnId` |
| Callback registrations, buffered undelivered events per turn | Pump side (Harness main-thread state) | Written/read only inside API calls and `update()` |
| Conversation contents | App thread via pump | Mutated only at terminal-event delivery |
| Command queue, event queue | Shared, internally synchronized | Sanctioned crossing points |
| Per-turn cancel flag (`atomic<bool>`) | Shared | Third sanctioned crossing point |
| `TurnId` | Immutable value | Freely copied everywhere |

The worker never touches callbacks or buffers; it emits events tagged with `TurnId`. The pump owns routing, buffering, and delivery. This split is what makes the §2 enumeration true.

### Send / cancel / shutdown

```mermaid
sequenceDiagram
    participant App as App thread (API + pump)
    participant CQ as Command queue
    participant W as Worker
    App->>CQ: SendTurn{id, request}
    W->>W: dequeue, run transfer (checks cancel flag at every I/O boundary)
    W-->>App: events{id, ...} via event queue, delivered in update()
    App->>App: turn.cancel() → sets atomic flag
    App->>CQ: Cancel{id} (covers still-queued turns)
    W-->>App: terminal event: Cancelled{id}
    App->>W: ~Harness(): request_stop() → abort all transfers, drain, join (bounded)
```

### Turn lifecycle (normative)

```mermaid
stateDiagram-v2
    [*] --> Queued: send()
    Queued --> AwaitingModel: worker picks up (FIFO)
    Queued --> Cancelled: cancel() before start (no I/O ever issued)
    AwaitingModel --> Streaming: first content event
    Streaming --> AwaitingTool: stop_reason tool_use
    AwaitingTool --> AwaitingModel: tool result submitted
    AwaitingModel --> Completed
    Streaming --> Completed
    AwaitingModel --> Failed
    Streaming --> Failed
    AwaitingTool --> Cancelled
    AwaitingModel --> Cancelled
    Streaming --> Cancelled
    Completed --> [*]
    Failed --> [*]
    Cancelled --> [*]
```

Exactly one terminal event per turn — never zero, never two. The remaining lifecycle contracts, each of which is a numbered requirement:

- **Conversation commits are transactional.** History is mutated only by the pump at terminal-event delivery: `Completed` commits the full exchange (user message, all tool rounds, final answer) atomically; `Failed`/`Cancelled` commit nothing — the Conversation is exactly as it was before `send()`, so retry-by-resend is trivially correct.
- **Detach semantics.** Dropping the handle detaches: the turn runs to termination, the Conversation still commits on completion. Events buffered for a destroyed handle are discarded at delivery time; the turn's *effects* survive, its *observability* doesn't.
- **Late attachment.** Callbacks attached after events began arriving receive buffered prior events in order — no races, no missed deltas.
- **Reentrancy.** Callbacks may call `send`, `cancel`, and registration APIs. Reentrant `update()` is forbidden and asserts. Tool registration changes take effect for subsequent turns, not in-flight ones.
- **Non-preemption.** The `update()` budget is a soft deadline checked *between* callbacks; an individual callback or tool handler is never preempted and may overrun the budget. The budget bounds Scry's scheduling, not user code.
- **Callback exceptions** propagate out of `update()` with the harness valid and the event counted delivered (§1).
- **Shutdown.** `~Harness()` cancels all turns, aborts transfers, joins the worker within a bound set by transport-abort latency, and discards undelivered events. No callback ever fires after destruction begins.

## 4. The Agentic Loop: Sans-I/O State Machine

The loop engine — the heart of the library — is written **sans-I/O**: a pure state machine that consumes events (*provider replied with tool call*, *tool result ready*, *stream ended*, *transport failed*) and emits commands (*issue this request*, *run this tool*, *deliver this to the app*), and performs **no I/O itself**. The worker thread is a thin driver that feeds it transport events and executes its commands.

Why this is the hill to defend:

- **Testability without a network.** The full agentic loop — multi-round tool use, retries, cancellation mid-tool-call, malformed model output — is tested by feeding event sequences and asserting command sequences. Deterministic, sub-millisecond tests for the most complex logic in the system.
- **Replayability.** A recorded event log reproduces any bug exactly. Given how nondeterministic LLM behavior is, deterministic *harness* behavior is the only debuggable posture.
- **The state machine is explicit, not emergent.** States (awaiting-model, awaiting-tool, retrying, cancelling, terminal) are a variant/enum with a drawn transition diagram, not an implicit property of nested callbacks. Illegal transitions are unrepresentable or assert.

Retry policy (backoff + jitter) lives inside the machine as state, driven by *time events* injected by the driver — the machine never sleeps, it requests "wake me at T." This keeps even timing testable with a fake clock.

## 5. Tool Registry: Type Erasure Below, Reflection Above

Two layers, one table (as settled in DESIGN.md §8):

**Lower layer — type erasure.** A registered tool is a record: name, description, schema (JSON string), and a type-erased callable (`json → expected<json, error>` in spirit; `std::move_only_function` so captures needn't be copyable). This is the `std::function`-style erasure idiom: the registry is runtime-uniform, closed to no one, and has zero knowledge of reflection.

**Upper layer — consteval code generation.** The P2996 layer is a compile-time *code generator* targeting the lower layer: given an args struct, `consteval` functions walk its members to (a) build the schema as a compile-time string and (b) instantiate a deserializer + invoker lambda that gets erased into the table like any hand-written tool. Practices:

- The reflection code lives in its **own header, behind a feature macro**, and touches nothing else. Severability is a build-time property, not a promise: CI builds the library both with and without it.
- **Concepts guard the gate.** The reflected-registration entry point is constrained to aggregate structs with supported member types, so misuse fails at the call site with a legible diagnostic, not in the guts of a `consteval` walk. Static asserts inside the walk carry human-readable messages ("member X of tool args must be string, number, bool, or reflected struct").
- **Schemas are computed once, at compile time.** No runtime schema caching, no static registries, no macro tricks — the schema string is a `constexpr` artifact of the type.
- Parameter descriptions use annotations (P3394) if the pinned toolchain has them, else a trait/customization-point specialization per args struct. The customization point follows the modern pattern (dedicated CPO / trait specialization), *not* ADL free functions, to keep lookup predictable.

## 6. Provider Adapters: Strategy at a Narrow Seam

One of the two sanctioned virtual interfaces. The pattern is classic **Strategy**: a small internal interface — translate neutral request → wire request, parse wire stream events → neutral events — with one implementation per dialect (Anthropic; OpenAI-compatible).

Discipline that keeps it clean:

- **The neutral model is the API of this seam.** Adapters see `Message`/`ContentBlock`/`ToolSchema` and nothing above; nothing above sees JSON or HTTP. Wire-format knowledge concentrated in one file per provider.
- **Adapters are stateless translators** where possible; stream-parsing state (partial SSE event, current content block index) is an explicit per-turn parser object, not adapter member state — one adapter instance serves many turns.
- Selection is config-driven via an internal factory keyed on dialect enum. No plugin registration machinery until a third-party provider actually needs it — **YAGNI applies to extension points too.**
- **Golden-file tests** per adapter: captured real wire payloads checked into the repo, asserted against neutral-model round-trips. This is the layer where upstream API drift bites, so tests are data, easy to re-capture.

## 7. Transport: RAII-Wrapped curl Behind an Injectable Seam

The second sanctioned interface, existing for one reason: **dependency injection of a fake transport in tests** (and of the sans-I/O driver's event source). Practices:

- libcurl used directly (not through a wrapper lib) for SSE control, but every curl object lives in a RAII wrapper with the curl types visible only in the `.cpp`. curl's C callbacks trampoline into C++ via the standard `void* userdata` → object pointer pattern, with all exceptions caught at the trampoline (C stacks must never unwind).
- **SSE parsing is a pure incremental function**: bytes in, zero-or-more events out, remainder buffered. No I/O, no allocation beyond the buffer — property-testable with randomly split byte chunks (the classic bug in SSE parsers is delimiter-across-chunk; the test generator targets it directly).
- `stop_token` plumbed into curl's progress callback for prompt cancellation of in-flight transfers.
- Connection reuse (curl multi/share) is an internal optimization invisible above the seam.

## 8. Errors as Values, Categorized Once

- Internal fallible paths return `std::expected<T, Error>`; `Error` is one struct with a category enum (auth, rate-limit, network, protocol, tool, resource, busy, cancelled) + message + provider-specific detail. One error type end-to-end — no per-layer error hierarchies to translate between. (`resource`: a NET-008 bound was exceeded; `busy`: a `send()` raced an in-flight turn on the same Conversation, THR-014.)
- The retry classifier (which categories are retryable) is a pure function owned by the loop state machine, tested as a table.
- At the boundary, errors become the `on_error` event. `errno`-style status polling is deliberately absent; there is exactly one way to learn of failure.

## 9. JSON and Dependency Policy

- **Glaze** for JSON (compile-time reflection alignment, header-only, fast); treated as an *internal* dependency — no Glaze types in public headers. The tool boundary's `json` value type is our own thin alias/wrapper so the JSON library remains swappable in principle.
- Dependency bar is high: curl, Glaze, and test frameworks. Each new dependency needs a written justification in this doc. Header hygiene enforced (IWYU in CI) so the PImpl firewall stays real.

## 10. Testing & Tooling Practices

- **The test pyramid mirrors the architecture:** sans-I/O machine tests (majority, no network, no threads) → adapter golden-file tests → transport tests against a local mock HTTP/SSE server → a thin end-to-end smoke suite against a real local model (Ollama/llama.cpp in CI, nightly not per-commit).
- Threaded code tested under **TSan and ASan in CI** from M0 — sanitizers are cheap the day the code is written and impossible to retrofit onto a flaky foundation. UBSan on the reflection layer especially.
- CI matrix: GCC trunk and clang-p2996 (reflection ON), plus a stable GCC/Clang (reflection OFF) proving severability. clang-format + clang-tidy configs checked in at M0; formatting arguments end on day one.
- **Warnings are errors** (`-Wall -Wextra -Wconversion -Wshadow`, the QA-006 set), from the first commit.

## 11. Evolution Register: Deliberate Simplifications and Their End States

Every "boring first" choice is recorded here with the condition that triggers evolution and the intended destination — so simplicity stays a decision, not an accident. Additions to this codebase that simplify deliberately must add a row.

| Simplification (now) | Trigger to evolve | Desired end state |
|---|---|---|
| Mutex + deque + condvar queues | Profiling shows queue contention or pump latency in a real app | Lock-free MPSC (commands) / SPSC (events) behind the same interface; interface designed for this swap from day one |
| One worker thread per Harness, one turn in flight per Conversation | A real app needs concurrent turns at scale | curl-multi–driven multiplexing of N turns on one worker; the actor + sans-I/O split means the machine layer is untouched |
| Blocking `send`-and-wait built on pump-until-complete | Coroutine-scheduler apps appear as users | `co_await`-able turn awaitable layered on the event queue; core remains callback/pump-based |
| Provider factory keyed on internal enum, no plugin API | A third-party provider that can't be upstreamed | Public adapter concept + registration hook; only then |
| Trait/customization-point parameter descriptions | Pinned toolchain gains P3394 annotations | Annotations in the args struct become the primary path; trait remains as override |
| No connection pooling beyond curl defaults | Measured connect/TLS overhead in streaming-heavy use | curl share/multi connection reuse, invisible above the transport seam |
| Linux + macOS only | Concrete Windows user demand | Windows reflection-OFF via clang; MSVC leg only if/when P2996 ships there |
| Reflection-ON CI legs on Linux only (PORT-005 as amended 2026-07) | A prebuilt or cheaply-cached P2996 toolchain for macOS exists | Full reflection ON/OFF matrix on both platforms |
| clang-p2996 CI leg is a non-gating probe on a community image (ADR 0004) | Spike A pins the reflection toolchain (owned/digest-pinned image or cached source build) | Both reflection legs (clang-p2996 + GCC trunk) gate per-commit |
| Serialized transfers: queued turns wait while the active turn awaits a main-thread tool | Serialized M1 scheduling measurably limits a real app | Tool-await releases the transfer slot under curl-multi multiplexing (same trigger as row 2) |

## 12. Pattern Summary

| Piece | Governing pattern / idiom |
|---|---|
| Public types | PImpl, value semantics, Rule of Zero |
| Concurrency | Actor model; message passing over variant commands/events; jthread + stop_token |
| Delivery | Single-threaded-by-construction pump with time budget |
| In-flight turns | Move-only handle + shared control block (future/stop_source school) |
| Agentic loop | Sans-I/O explicit state machine; time as injected events |
| Tool registry | Type erasure (move_only_function) below; consteval codegen above; concepts at the gate |
| Providers | Strategy at a narrow seam; stateless translators; golden-file tests |
| Transport | RAII curl, C-callback trampolines, injectable seam; pure incremental SSE parser |
| Errors | expected-based values, single categorized type, one failure channel |
| Extensibility | Callables and config, not inheritance; YAGNI on plugin machinery |
