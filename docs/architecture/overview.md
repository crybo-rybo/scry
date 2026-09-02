# Architecture Overview

Companion to the [design overview](../design/overview.md). The design
documentation says *what* the pieces are; this section says *how* each piece is
built and why. Deliberately no class listings or method signatures; the public
headers are the API source of truth.

---

## 1. Library-Wide Principles

These apply everywhere and settle arguments before they start.

**Value semantics at the boundary, explicit ownership inside.** Public types are
cheap-to-move value types or lightweight handles. Exclusively owned resources
use `std::unique_ptr` or dedicated RAII wrappers. Shared lifetime is deliberate
and enumerated: command/event queues, the per-turn cancellation flag, and
immutable accepted-turn history/schema blocks cross the worker boundary; a
Conversation handle and its live pump route share Conversation state; and a
pump-side turn route is owned by the Harness and observed weakly by the Turn
handle. Snapshot blocks are immutable while shared, and the pump reseats a
private history block copy-on-write before mutation. Tool handlers are never
shared: each one stays in the pump-side snapshot of the turn that captured it.
Raw pointers are non-owning observers only, never stored across a suspension
point.

**Rule of Zero.** Types define no special member functions unless they manage a resource directly; resource management is pushed into dedicated RAII wrappers (curl handles, threads, queues) so everything above them defaults.

**PImpl on stateful handles; plain values for contracts.** The four stateful handle types (Conversation, ToolRegistry, Turn, Harness) hold a single pointer to an implementation — ABI stays stable across internal refactors, and compile-time cost for consumers stays flat. Everything else the user touches is an ordinary value type: configuration aggregates (designated-initializer friendly), errors, options, enums, event payloads, and the Scry-owned JSON boundary type. The binding rule underneath both is: **no third-party types in public headers, ever** (enforced by include audit). `Harness` is constructed directly from `Config` and is the single owner of provider/auth/connection state; a separate Client handle would add lifetime ambiguity without an independent responsibility. The indirection cost is irrelevant next to network latency — this library's hot path is measured in milliseconds, not nanoseconds.

**No singleton carries library state.** Everything that can vary hangs off a
`Harness` instance, so two Harnesses with different providers remain isolated.
The one process-wide capability is libcurl global state: a function-static RAII
owner performs one initialization attempt, caches that first result, cleans up
immediately when post-init capability validation fails, and otherwise cleans
up exactly once at static teardown. Harness construction and destruction never
churn that global runtime.

**Semantic failures are values; callback exceptions stay synchronous.** Internals may use whatever is idiomatic for the dependency at hand, but Scry-originated semantic and operational failures never throw across the public boundary. Immediate rejection reports through `std::expected`; failure after a turn is accepted reports through the `on_finished` callback supplied to `send()`, which receives the error in place of a completion. Allocation and standard-library construction failure (`std::bad_alloc`) are **excluded from the contract** — Scry does not pretend to survive OOM, and smearing `noexcept`+expected over every allocating call would buy nothing. Two hard rules stand regardless: nothing ever throws *across* the worker/main thread boundary, and tool-handler exceptions are caught at the dispatch site and converted into tool-error results returned to the model. User callbacks should not throw; if one does, the exception propagates synchronously out of `update()` to the app with the Harness left in a valid state and the event counted as delivered (see [runtime architecture](runtime.md)).

**Concepts over inheritance in templates, interfaces only at seams.** Virtual dispatch appears in exactly two places—the provider adapter and transport described in [tools, providers, and transport](tools-and-providers.md)—both internal. The public API has no inheritable types; extension points are callables and config, not subclassing.

**C++ standard posture.** Core library targets C++23 (`std::expected`, deducing
this where useful). Callable boundaries use Scry's small move-only
`UniqueFunction` because supported macOS standard libraries do not yet
consistently ship `std::move_only_function`; the boundary remains move-only
rather than silently becoming copy-only on one platform. The reflection layer
remains an isolated, severable, experimental C++26 component (see
[tools architecture](tools-and-providers.md)) and is
not part of the stable runtime surface.

## 2. The Concurrency Architecture: Actor, Not Locks

The single most important structural decision. The worker thread is an
**actor**: it exclusively owns all mutable networking and loop state, and (bar
the per-turn cancellation atomic; see [runtime architecture](runtime.md)) the
only way anything crosses the thread
boundary is **message passing** through two queues. Commands cover send,
cancel, tool result, and shutdown; events cover text deltas, tool-call
requests, completions, and errors. Every message type lives on those two
queues; nothing gets its own side channel.

Practices that follow:

- **Enumerated shared state, no user-visible locks.** There is no mutex a user callback can deadlock against. Mutable cross-thread state is limited to exactly three internally synchronized objects: the command queue, the event queue, and one atomic cancellation flag per turn. Accepted-turn commands also carry shared immutable history and tool-schema blocks; the worker only reads them, and the pump reseats any still-shared history block copy-on-write before mutation. Everything else — worker-side state and pump-side state — is exclusively owned (see the [runtime ownership table](runtime.md)), and the worker addresses turns only by immutable `TurnId`. Pump-side-only lifetime sharing is not cross-thread state. This enumeration *is* the invariant TSan enforces; anything not on the list found crossing threads is a bug by definition.
- **Messages are immutable values.** Commands and events are `std::variant` of small structs, moved (never copied) through the queue. Variant + `std::visit` gives exhaustive handling — adding an event type breaks the build until every consumer handles it. This is the same closed-set-of-alternatives reasoning that picks variant over inheritance everywhere in this codebase.
- **Queue implementation: boring first.** Mutex + `std::deque` + condition variable, wrapped behind a minimal Scry-owned interface so a lock-free MPSC queue can be swapped in *if profiling ever demands it*. Premature lock-free is how libraries acquire unfixable bugs.
- **Two cancellation mechanisms, deliberately separate.** The worker's `std::jthread` `stop_token` means one thing only: **Harness shutdown**. Per-turn cancellation is a distinct per-turn `std::atomic<bool>`, checked at every I/O boundary and plumbed into transport progress callbacks. Both are cooperative, never `pthread_cancel`-style. Conflating them was an early ambiguity: shutdown must abort *all* turns and join; cancelling one turn must not disturb its neighbors.
- **The pump is the contract.** `update()` drains the event queue and invokes callbacks on the calling thread, under an optional time budget (deadline-checked between events, excess rolls to the next tick, with one event and one callback always processed so no budget starves the pump). Because *all* callbacks fire there, user code is single-threaded by construction. This is the harness's most sacred invariant; everything else may change.
- **Backpressure by design.** Streaming deltas are coalesced worker-side (one aggregated text event per pump interval, not per token) so a fast stream cannot flood the queue or starve the frame budget. Coalescing is not the memory bound: configurable byte ceilings on pending turns, per-turn queued events, responses, tool payloads, and conversations are the hard limits.

**Blocking-mode escape hatch:** a synchronous `send`-and-wait exists for CLI tools and tests, implemented *on top of* the async machinery (pump-until-complete internally), never as a second code path.
