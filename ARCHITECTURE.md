# Scry — Architectural Patterns & Implementation Practices

Companion to [DESIGN.md](DESIGN.md). That document says *what* the pieces are; this one says *how* each piece is built — the C++ design patterns, idioms, and practices we commit to, and why. Deliberately no class listings or method signatures; those belong to the M0 header sketch.

---

## 1. Library-Wide Principles

These apply everywhere and settle arguments before they start.

**Value semantics at the boundary, ownership inside.** Public types are cheap-to-move value types or lightweight handles. Internally, every resource has exactly one owner (`std::unique_ptr`, RAII wrappers); `shared_ptr` appears only where lifetime is genuinely shared across threads (see §3, Turn handles). Raw pointers are non-owning observers only, never stored across a suspension point.

**Rule of Zero.** Types define no special member functions unless they manage a resource directly; resource management is pushed into dedicated RAII wrappers (curl handles, threads, queues) so everything above them defaults.

**PImpl on every public type.** All public types hold a single pointer to an implementation. Three payoffs: no third-party headers (curl, JSON) leak into the public include path; ABI stays stable across internal refactors; compile-time cost for consumers stays flat. The cost (one indirection, no inlining across the boundary) is irrelevant next to network latency — this library's hot path is measured in milliseconds, not nanoseconds.

**No singletons, no globals, no static init order problems.** Everything hangs off a `Harness` instance. Two harnesses in one process (e.g., different providers) must just work. The one unavoidable global — libcurl's `curl_global_init` — is wrapped in a reference-counted RAII guard (Meyers-style function-local static, initialized on first Harness).

**Exceptions stop at the membrane.** Internals may use whatever is idiomatic for the dependency at hand, but the public boundary is exception-free by contract: fallible operations report through `std::expected` or the `on_error` callback. Two hard rules: nothing ever throws *across* the worker/main thread boundary, and tool-handler exceptions are caught at the dispatch site and converted into tool-error results returned to the model (the model can often self-correct; the app should not crash because the LLM passed a bad argument).

**Concepts over inheritance in templates, interfaces only at seams.** Virtual dispatch appears in exactly two places (provider adapter, transport — §6, §7), both internal. The public API has no inheritable types; extension points are callables and config, not subclassing.

**C++ standard posture.** Core library targets C++23 (`std::expected`, `std::move_only_function`, deducing this where useful). The reflection layer is an isolated C++26 module of the codebase, kept severable (see §5).

## 2. The Concurrency Architecture: Actor, Not Locks

The single most important structural decision. The worker thread is an **actor**: it exclusively owns all mutable networking and loop state, and the only way anything crosses the thread boundary is **message passing** through two queues — a command queue in (send, cancel, shutdown) and an event queue out (deltas, tool requests, completions, errors).

Practices that follow:

- **No shared mutable state, no user-visible locks.** There is no mutex a user callback can deadlock against. The queues are the *only* synchronization points, which makes the concurrency story auditable in one file.
- **Messages are immutable values.** Commands and events are `std::variant` of small structs, moved (never copied) through the queue. Variant + `std::visit` gives exhaustive handling — adding an event type breaks the build until every consumer handles it. This is the same closed-set-of-alternatives reasoning that picks variant over inheritance everywhere in this codebase.
- **Queue implementation: boring first.** Mutex + `std::deque` + condition variable, wrapped behind our own minimal interface so a lock-free MPSC queue can be swapped in *if profiling ever demands it*. Premature lock-free is how libraries acquire unfixable bugs.
- **`std::jthread` + `std::stop_token`** for worker lifetime; `stop_token` is threaded through blocking waits and the transport layer so shutdown and per-turn cancellation share one mechanism. Cancellation state per turn is a `std::atomic<bool>` checked at every I/O boundary — cooperative, never `pthread_cancel`-style.
- **The pump is the contract.** `update()` drains the event queue and invokes callbacks on the calling thread, under an optional time budget (deadline-checked between events, excess rolls to the next tick). Because *all* callbacks fire there, user code is single-threaded by construction. This is the harness's most sacred invariant; everything else may change.
- **Backpressure by design.** Streaming deltas are coalesced worker-side (one aggregated text event per pump interval, not per token) so a fast stream cannot flood the queue or starve the frame budget.

**Blocking-mode escape hatch:** a synchronous `send`-and-wait exists for CLI tools and tests, implemented *on top of* the async machinery (pump-until-complete internally), never as a second code path.

## 3. Handle Pattern for In-Flight Work

A `Turn` is a **handle**: a small value object referring to shared turn state, not the state itself. The pattern:

- Turn state lives in a control block owned jointly by the handle and the worker (`shared_ptr` internally — one of the two sanctioned uses). The handle going out of scope must be *safe and meaningful*: default is detach (turn completes, events still delivered), with explicit cancel available.
- Callbacks registered on the handle are stored in the control block and invoked only from the pump (§2), so registration order and thread guarantees hold no matter when the user attaches them — including *after* events have started arriving (late-attached callbacks receive buffered events; no races, no missed deltas).
- Handles are move-only. Copying a handle to in-flight work invites double-cancel ambiguity; if two parts of the app need visibility, that's an app-level decision made explicit.

This is deliberately the `std::future`/`std::stop_source` school of design: small, thread-safe-by-narrowness, no behavior hidden in destructors beyond a documented detach.

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

- Internal fallible paths return `std::expected<T, Error>`; `Error` is one struct with a category enum (auth, rate-limit, network, protocol, tool, cancelled) + message + provider-specific detail. One error type end-to-end — no per-layer error hierarchies to translate between.
- The retry classifier (which categories are retryable) is a pure function owned by the loop state machine, tested as a table.
- At the boundary, errors become the `on_error` event. `errno`-style status polling is deliberately absent; there is exactly one way to learn of failure.

## 9. JSON and Dependency Policy

- **Glaze** for JSON (compile-time reflection alignment, header-only, fast); treated as an *internal* dependency — no Glaze types in public headers. The tool boundary's `json` value type is our own thin alias/wrapper so the JSON library remains swappable in principle.
- Dependency bar is high: curl, Glaze, and test frameworks. Each new dependency needs a written justification in this doc. Header hygiene enforced (IWYU in CI) so the PImpl firewall stays real.

## 10. Testing & Tooling Practices

- **The test pyramid mirrors the architecture:** sans-I/O machine tests (majority, no network, no threads) → adapter golden-file tests → transport tests against a local mock HTTP/SSE server → a thin end-to-end smoke suite against a real local model (Ollama/llama.cpp in CI, nightly not per-commit).
- Threaded code tested under **TSan and ASan in CI** from M0 — sanitizers are cheap the day the code is written and impossible to retrofit onto a flaky foundation. UBSan on the reflection layer especially.
- CI matrix: GCC trunk and clang-p2996 (reflection ON), plus a stable GCC/Clang (reflection OFF) proving severability. clang-format + clang-tidy configs checked in at M0; formatting arguments end on day one.
- **Warnings are errors** (`-Wall -Wextra -Wconversion`), from the first commit.

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
