# ADR 0001: Public Object Graph and Turn Lifetimes

- Status: Accepted
- Date: 2026-07-17

## Context

The pre-M0 design described both `Client` and `Harness` as owners of provider and
connection state, allowed a `ToolRegistry` to attach to either a Client or a
Conversation, and did not fully specify how Turn callbacks survive handle and
Harness lifetimes. Those choices would make the first public headers encode
ambiguous ownership.

## Decision

- `Harness::create(Config)` is the only configured runtime factory. There is no
  public Client type.
- A Harness owns its provider/auth/connection state, worker actor, pump state,
  and one ToolRegistry.
- M1 may validate and store inert registry entries. Beginning in M2, `send()`
  snapshots entries into the accepted turn and reentrant registry changes
  affect later turns only.
- Conversation is a move-only PImpl handle. A Conversation may have at most one
  queued or active turn.
- Turn is a move-only PImpl handle containing a TurnId, a shared cancellation
  flag, and a weak route to Harness-owned pump registration state.
- Dropping a Turn detaches. It does not cancel or block, and callbacks already
  registered with the Harness remain active.
- A Turn that outlives its Harness may still set its cancellation flag.
  Registration fails with `invalid_state`; no callback can fire after Harness
  destruction begins.
- Immediate validation, admission, and registration failures return
  `std::expected`. Once accepted, a turn has one asynchronous failure channel:
  `on_error`.

## Consequences

The object graph has one unambiguous runtime owner and no global or
Conversation-local tool registry. Turn lifetime is safe without sharing worker
state or callback storage across threads. The cost is that sharing connection
state across Harness instances is not an M0 feature; curl-level reuse remains an
internal optimization if measurement later justifies it.

## Amendment: Callbacks at Send (2026-08-03)

### Amendment context

The original decision put callback registration on `Turn`, requiring a weak
registration route, fallible post-acceptance mutation, and replay of events
that arrived before registration. The implemented runtime now has a simpler
ownership boundary: an application knows its observers when it submits a turn,
while the returned handle is needed only for identity and cancellation.

### Amended decision

This amendment supersedes the original callback-registration portions of the
Decision above; the Harness, ToolRegistry, Conversation, snapshot, and detach
decisions remain accepted.

- `Harness::send()` accepts a `TurnCallbacks` value. Accepting the turn moves
  that value infallibly and atomically into the Harness-owned pump route before
  the worker command is published, so no event can precede callback attachment.
- The accepted callback set is immutable. `Turn` exposes identity and
  cooperative cancellation only; there is no late callback registration or
  replay. An event with no matching callback is released immediately, while
  terminal processing still commits or rolls back the Conversation and clears
  its busy state.
- Success, failure, and cancellation share one optional terminal callback:
  `on_finished(Result<Completion>)`. When non-empty, it is invoked exactly once
  per accepted turn unless Harness destruction begins first. Empty callbacks
  are legal and request no application delivery.
- `on_text_delta(std::string_view)` and `on_tool_call(const ToolCall&)` borrow
  their arguments for the invocation. `on_finished` receives its result by
  value.
- Callbacks may call `send()`, `cancel()`, and tool registration. Reentrant
  `update()` performs no work, leaves queued events untouched, and returns
  `callbacks_delivered == 0` with `budget_exhausted == true`.
- `Turn::cancel()` returns `true` only for the call that issues a cancellation
  request and returns `false` after a prior request or pump-side terminal
  processing. A Turn that outlives its Harness still has only the original
  harmless atomic fallback available.

### Amendment consequences

Callback lifetime is now unambiguous at acceptance, and the replay buffer and
fallible observer-registration surface disappear. Dropping a Turn still
detaches without suppressing route-owned callbacks. Consumers that need mutable
observer state capture indirection owned by the application; a future dynamic
subscription API requires a separate lifetime and backpressure decision rather
than silently restoring callback methods on `Turn`.
