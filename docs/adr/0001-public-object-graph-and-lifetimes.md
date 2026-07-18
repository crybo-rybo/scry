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
