# ADR 0006: M2 Agentic Tool Loop

- Status: Accepted
- Date: 2026-07-18

## Context

M1 deliberately stopped at validated, inert explicit-schema registrations.
M2 must make those registrations executable without weakening Scry's
main-thread callback contract, serialized scheduling baseline, sans-I/O
machine, or transactional Conversation commits. The requirements also defer
registry replacement/removal semantics and the exact multi-call tool-round
behavior to this milestone.

Tool handlers are move-only and may capture application state. Copying a
mutable registry into accepted turns would either be impossible or would share
mutation accidentally. Executing handlers on the worker would violate the
default threading contract. Committing intermediate rounds as they happen
would leave a failed or cancelled turn partially visible in Conversation
history.

## Decision

- The explicit-schema registry remains additive-only. `add()` rejects duplicate
  names; M2 adds no replacement or removal API. A concrete hot-reload or
  dynamic-plugin use case must define those semantics before that surface is
  introduced. The Harness-owned registry cannot be moved out through the
  public API.
- `add()` parses the explicit input schema immediately, requires a JSON object,
  and stores its canonical form. M2 deliberately validates syntax and the root
  kind only; the supported schema vocabulary is defined with the M3 type
  mapping.
- Each registration becomes an immutable shared record containing its
  definition and shared ownership of its move-only handler. `send()` copies a
  vector of record owners into the accepted turn. Later registration creates a
  new record and affects later turns only.
- Registry records stay on the app/pump side. Only neutral tool schemas cross
  to the worker in `ModelRequest`; tool calls and results cross the existing
  queues as Scry-owned values.
- One assistant response containing one or more tool calls is one tool round.
  The machine validates stable, non-empty, previously undispatched call IDs,
  publishes every call in provider order exactly once, and enters the explicit
  `AwaitingTool` state.
- The worker submits every tool-call event from that response to the
  worker-to-pump queue as one atomic batch. If the whole batch cannot fit, no
  prefix is published and no handler executes.
- Default handlers execute synchronously inside `Harness::update()` on its
  calling thread. The pump posts each result back to the worker. An optional
  `on_tool_call` observer runs after the result has been posted, so an observer
  exception can propagate from `update()` without stranding the active turn.
- Cancellation observed before a handler is dispatched skips that handler.
  Because handlers are non-preemptive, cancellation requested during a handler
  takes effect after that handler returns: its result and all remaining calls
  in the batch are suppressed, no model resend occurs, and the turn terminates
  cancelled.
- Unknown tools, handler-returned errors, handler exceptions, and invalid JSON
  outputs become bounded `is_error` tool results sent back to the model.
  Framework resource-limit violations fail the accepted turn with
  `resource_limit`; they are not disguised as model-visible tool failures.
  That fatal framework path is latched by the pump and suppresses every later
  handler in the batch.
  A returned `Error` may contribute a sanitized, bounded model-visible
  diagnostic. Caught exceptions always use a generic message and never expose
  `what()`.
- The machine waits for all results, preserves provider call order regardless
  of result arrival order, appends one assistant tool-call message and one user
  tool-result message, then issues the next model request automatically.
- `max_tool_rounds` counts assistant responses containing tool calls. A response
  that would exceed the cap terminates with `max_tool_rounds` before dispatching
  any call from that response.
- Tool-call IDs are unique for the full turn. Reuse is a protocol failure and
  can never dispatch a handler twice.
- At-most-once dispatch is scoped to one accepted turn and one tool-call ID; it
  is not an external transaction. Side-effecting tools must carry an
  app-owned idempotency/operation key, durably record key→result around the
  effect, and reconcile ambiguous outcomes before the app resubmits a failed
  or cancelled user request.
- Before any tool is published, the machine rejects mismatches between
  `finish_reason` and content, empty IDs or names, duplicate IDs, invalid tool
  arguments, and unsupported content-block roles. Protocol-invalid batches
  therefore have no handler side effects.
- Retry attempt and elapsed-time caps apply independently to each outbound
  model request. `Completion::attempt_count` reports the total transport
  attempts across the full turn, and usage totals accumulate across rounds.
- The Conversation's remaining payload capacity becomes one machine-owned
  exchange budget at admission. Every assistant tool-call message, individual
  tool result, and final assistant message is reserved cumulatively before
  dispatch, resend, or commit. A resource failure keeps the provider request
  ID available at the response/tool boundary.
- The pump commits the initial user message, every assistant/tool-result round,
  and the final assistant response together only when the terminal completion
  event is accepted. Failure or cancellation commits none of them.
- Conversation persistence uses a versioned Scry-owned JSON document exposed as
  `Result<Json> Conversation::to_json() const` and
  `static Result<Conversation> Conversation::from_json(const Json&)`. Version 1
  preserves the system prompt and all committed neutral content blocks without
  exposing Glaze types. Busy state and uncommitted turn data are never
  serialized. Malformed documents, unknown versions or fields, and
  role/content mismatches return `invalid_config`.

## Consequences

The worker retains the serialized turn slot while waiting for main-thread tool
execution, as required by the M2 scheduling baseline. The machine remains
deterministic and independent of queues, clocks, providers, and handlers.
Reentrant registration is safe because accepted turns never inspect the live
registry again.

Additive-only mutation is intentionally smaller than a hot-reload registry but
has unambiguous lifetime and snapshot behavior. Worker-thread handlers remain
an M4 opt-in layered onto the same immutable records; they do not require a
second registration table or a parallel agent loop.
