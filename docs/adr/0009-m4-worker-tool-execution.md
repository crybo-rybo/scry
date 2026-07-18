# ADR 0009: M4 Worker-Thread Tool Execution

- Status: Accepted
- Date: 2026-07-18

## Context

M2 executes every tool handler synchronously inside `Harness::update()`. That
default is correct for handlers touching host-owned game or GUI state, but a
thread-safe slow handler can overrun the host's frame budget. M4 adds the
per-tool worker-thread opt-in required by THR-021 without creating another
agent loop, weakening immutable accepted-turn snapshots, or moving callbacks
off the update thread.

An arbitrary C++ callable is non-preemptive. `std::stop_token` can request
cooperation but cannot safely terminate a handler that ignores it, and Scry
does not inject a stop token into the public handler signature. The teardown
contract must therefore distinguish Scry-owned blocking operations from
application-owned worker handlers.

## Decision

### Public registration surface

The stable C++23 surface adds:

```cpp
enum class ToolExecution : std::uint8_t {
  app_thread,
  worker_thread,
};

struct ToolRegistrationOptions {
  ToolExecution execution{ToolExecution::app_thread};
};

Status ToolRegistry::add(ToolDefinition, ToolHandler);
Status ToolRegistry::add(ToolDefinition, ToolHandler,
                         ToolRegistrationOptions);
```

The existing two-argument entry point retains app-thread behavior; the
three-argument overload selects a mode without replacing the existing ABI.
`ToolDefinition` remains only provider-visible metadata; execution policy is
registration behavior, not schema data. The reflected `add<Args>` accepts and
forwards the same optional argument after the handler, so reflected and
explicit tools share one policy and one runtime.

### Ownership and snapshots

No new synchronized cross-thread object is introduced. The existing command
queue carries registration, send, execution, result, and cancellation
commands; the event queue carries tool calls, worker acknowledgements, and
terminal/output events; the per-turn atomic remains cancellation's fast path.

The app side keeps each immutable definition and execution mode. An app-thread
record also owns its handler. A worker-thread handler moves once through a
FIFO registration command into a worker-owned handler table and is never
shared back across the boundary. Registration remains additive-only and
duplicate validation remains synchronous.

An accepted turn snapshots definitions and execution modes. Only neutral
schemas and the names of worker-executed tools cross with the send command.
The live registry is never consulted after acceptance. FIFO command ordering
guarantees a worker registration is installed before a subsequently accepted
turn can request it.

### Ordered execution

The machine and provider still see one ordered tool-call batch. The worker
publishes the complete batch atomically before any handler runs. The pump
processes calls in provider order:

- app-thread mode invokes the snapshotted handler during `update()`, then posts
  its result exactly as in M2;
- worker-thread mode posts an execute command and pauses later calls from that
  batch for the route; and
- the worker invokes its owned handler, applies the result to the same
  `TurnMachine`, and emits a small accepted-result acknowledgement before
  processing commands caused by that result.

The acknowledgement lets the pump release the per-route gate, update its
remaining exchange accounting, and invoke the optional tool observer on the
update thread after the machine has accepted the result. A later call may then
run. A fatal framework failure publishes the existing terminal error and no
acknowledgement, so every later handler remains suppressed.

Unknown tools, returned errors, exceptions, invalid JSON, and result byte
limits use the same dispatch and sanitization rules in both modes. The machine
remains the authoritative cumulative exchange-budget gate. App-side mirrored
accounting is updated from the acknowledged canonical worker result before a
later mixed-mode call is admitted.

Worker-mode execution begins only because `update()` delivered its tool-call
event. It is never started eagerly from the network thread. A worker result
returns through the event queue, so applications must continue pumping to
advance a mixed or all-worker batch and receive observers or terminal events.
Exactly one worker-owned operation runs at a time under the existing
serialized Harness actor.

Opting in asserts that the handler and every captured object are safe to use
on the worker. The handler must not call app-thread-owned Harness,
Conversation, or ToolRegistry operations; shared application state requires
application-owned synchronization. App-state tools keep the default mode.

### Cancellation, observers, and teardown

Cancellation observed before dispatch suppresses either mode. App-thread
handlers remain non-preemptive; cancellation during one takes effect after it
returns and suppresses its result and later calls. Worker-thread handlers are
also non-preemptive. Cancellation requested while one runs sets the existing
atomic immediately; after the handler returns, its result and later calls are
suppressed and the turn terminates cancelled.

Every user callback, including `on_tool_call`, still executes only inside
`update()` on its caller's thread. Worker mode changes the handler thread, not
the observer thread.

Harness teardown requests worker stop and aborts Scry-owned transport waits
within the configured bounds. A currently executing application handler
cannot be forcibly stopped safely. Opting a handler into worker mode therefore
places a MUST on the application: the handler must return within the
application's required teardown bound and must not wait indefinitely. Its
execution time is excluded from Scry's Scry-owned shutdown-time guarantee.
The destructor joins after that handler returns and fires no callbacks once
destruction begins.

## Verification

M4 verification includes:

- public and reflected compile examples proving the default and explicit modes;
- thread-ID assertions for both modes and update-thread observer delivery;
- immutable accepted-turn snapshots and FIFO registration-before-send;
- all-worker and mixed ordered batches, canonical results, observer timing,
  cumulative budgets, and automatic resend through the existing machine;
- unknown/error/exception/invalid-JSON/result-limit paths with fatal
  suppression of later calls;
- cancellation before and during each mode, queued-turn serialization, and
  detached-turn behavior;
- worker-mode shutdown with a bounded cooperating handler plus an explicit
  documented non-cooperating-handler exclusion; and
- the full threaded suite under TSan.

## Consequences

Worker mode is a latency isolation option, not parallel tool execution. This
keeps the actor and machine simple, preserves deterministic provider order,
and does not introduce a second pool or scheduler. Parallel handlers would
need a separate resource, ordering, cancellation, and side-effect contract.

Handlers that need cancellable long work should own their own cooperative
operation and return promptly; adding a public handler stop token or async
handler result is deferred until a concrete use case justifies a larger
callable boundary.
