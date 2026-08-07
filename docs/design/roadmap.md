# Design: Future Directions and Scope

## 11. Future Directions

These are directions under consideration, not commitments. Earlier open
questions are settled and documented: the concurrency baseline, the JSON
library (Glaze — [dependency architecture](../architecture/dependencies-and-errors.md)),
and the HTTP library (libcurl direct —
[transport architecture](../architecture/tools-and-providers.md)). What remains
under consideration:

1. **Asynchronous / deferred tool results** — a handler that accepts a call,
   returns immediately, and completes its result on a later `update()`. This is
   the intended answer for a genuinely slow tool now that handlers all run on
   the app thread (see [runtime behavior](runtime-behavior.md)): it keeps app
   callbacks on the app's own thread instead
   of hiding a worker behind them, but it needs its own cancellation, ordering,
   and budget rules ratified before it ships.
2. **Structured output** — reflected structs also enable "answer as this type" (schema-constrained responses). A natural post-1.0 feature; the `Turn` surface keeps the door open.
3. **Coroutine sugar** — `co_await harness.send(...)` for apps with coroutine schedulers. Tracked in the evolution register; layered over the event queue later.

## 12. Scope

**Shipped in v0.1.0.** The C++23 runtime — Config, Conversation, ToolRegistry,
Turn, Harness — with the streaming agentic tool loop on a sans-I/O machine,
explicit-schema tools dispatched from `update()`, retries and cancellation,
transactional history with versioned (pre-1.0 unstable) persistence, and two
config-selected provider dialects: Anthropic Messages and the documented
OpenAI-compatible Chat Completions subset. Alongside it: the optional,
experimental GCC 16 `scry::reflection` component, and opt-in showcase examples
— a Dear ImGui chat panel and a deterministic grid world where the model drives
an NPC through explicit tools — that add no public, installed, or exported
surface.

**Not shipped, in rough order of demand.** The asynchronous/deferred
tool-result API and structured output from §11; curl-multi multiplexing of
concurrent turns; coroutine-awaitable turns; Windows support. Each has a trigger
and an intended end state in the
[evolution register](../architecture/quality-and-evolution.md) — nothing here is
scheduled, and nothing here is promised before 1.0.
