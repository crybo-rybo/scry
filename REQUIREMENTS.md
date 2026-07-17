# Scry — Requirements Register

**This document is normative.** Where prose in [DESIGN.md](DESIGN.md), [ARCHITECTURE.md](ARCHITECTURE.md), or [ENGINEERING.md](ENGINEERING.md) conflicts with this register, the register wins; the other documents provide rationale and context. Keywords MUST, MUST NOT, SHOULD, and MAY follow RFC 2119. Every requirement carries the milestone it lands in (per DESIGN.md §12) and how it is verified — "planned" until the verifying test or gate exists, at which point the row is updated to name it. A requirement with no credible verification path is a design smell and gets reworked, not waived.

ID scheme: `SCRY-<AREA>-NNN`, abbreviated to `<AREA>-NNN` in the tables below. IDs are permanent; withdrawn requirements are struck, never reused.

---

## API — Public Surface (SCRY-API)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| API-001 | MUST | The public API surface is limited to a small set of value/handle types (Client, Conversation, ToolRegistry, Turn, Harness); no public type is designed for inheritance. | M0 | Header review; no virtual/protected in public headers (grep gate) |
| API-002 | MUST | No third-party types or headers (curl, Glaze) appear in the public include path. The five stateful handles (Client, Conversation, ToolRegistry, Turn, Harness) use PImpl; configuration, errors, options, enums, event payloads, and the Scry-owned JSON boundary type are plain value types. | M0 | IWYU + include-graph CI check; header review |
| API-003 | MUST | The public boundary is exception-free: failures surface via `std::expected` or the error callback, never by throw. | M1 | API tests exercising all failure paths; no `throw` reaches boundary under ASan/UBSan suites |
| API-004 | MUST | Server/model configuration (base URL, auth, model, sampling params, dialect) is a plain `Config` value aggregate; switching providers or local servers requires no code changes. | M1 (shape); M4 (second dialect proves it) | Integration test: same app code against two dialects via config only |
| API-005 | MUST | The library never owns `main()`, never spins an event loop the app must join, and imposes no lifecycle on the host. | M0 | Design invariant; example apps demonstrate integration into pre-existing loops |
| API-006 | MUST | Multiple Harness instances in one process (including different providers) work independently; no singletons or mutable globals beyond the ref-counted curl-global guard. | M1 | Integration test with two concurrent Harnesses; symbol audit |
| API-007 | SHOULD | Conversation history is serializable/deserializable for app-side persistence. | M2 | Round-trip unit test |
| API-008 | MUST | A synchronous send-and-wait convenience exists, implemented on top of the async path (not a second code path). | M1 | Unit test; code review invariant |
| API-009 | MUST NOT | The library does not provide prompt-template/chain DSLs and is not an inference engine. | — | Scope gate at review |
| API-010 | MUST | Fallible construction (Harness/Client from Config) uses `noexcept`-path factories returning `std::expected`; semantic failures never throw. Allocation failure (`std::bad_alloc`) is excluded from the exception-free contract. | M0 | API tests over invalid configs; header review |
| API-011 | MUST | Conversation commits are transactional: history is mutated only by the pump at terminal-event delivery. Completion commits the full exchange (including tool rounds) atomically; error/cancellation commits nothing. | M1 | Unit tests asserting history across all three terminal paths |

## Threading & Concurrency (SCRY-THR)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| THR-001 | MUST | No public API call blocks the calling thread on network I/O; all blocking work happens on a harness-owned worker thread. | M1 | Integration test with watchdog timing on API calls |
| THR-002 | MUST | All user callbacks (turn events and default-mode tool handlers) execute only inside `update()`, on the thread calling it. | M1 | Thread-ID assertions in integration tests; TSan suite |
| THR-003 | MUST | Exactly three internally-synchronized objects cross the thread boundary: command queue, event queue, and one atomic cancellation flag per turn. All other state is exclusively owned per the ARCHITECTURE.md §3 ownership table; the worker addresses turns only by immutable TurnId. | M1 | TSan on all threaded tests, per commit; ownership review against the table |
| THR-004 | MUST | `update()` honors an optional caller-supplied time budget; undelivered events roll to the next call, none are lost. | M1 | Unit test with fake clock + event counting |
| THR-005 | MUST | Streaming deltas are coalesced (at most one aggregated text event per pump interval) so token rate cannot flood the queue. | M1 | Unit test with high-rate fake stream |
| THR-006 | MUST | `Turn::cancel()` is safe to call from the app thread at any time, including after completion; cancellation is cooperative and aborts in-flight transfers promptly. | M1 | Integration test: cancel at randomized points (incl. mid-stream, mid-tool) under TSan |
| THR-007 | MUST | Dropping a Turn handle detaches (turn continues, events still deliverable); it never blocks or cancels implicitly. | M1 | Unit test; destructor audit |
| THR-008 | MUST | Turn handles are move-only. | M0 | `static_assert` in tests |
| THR-009 | MUST | Callbacks attached after events have begun arriving receive buffered prior events; no events are raced past or dropped. | M1 | Unit test attaching late at randomized points |
| THR-010 | MUST | Nothing throws across the thread boundary; worker-side failures become error events. | M1 | Failure-injection tests under sanitizers |
| THR-011 | MUST | Tool handlers execute on the app thread inside `update()`. (Opt-in worker-thread execution is THR-021, M4.) | M2 | Thread-ID assertions |
| THR-012 | MUST | Worker lifetime uses `std::jthread`; the worker `stop_token` signals Harness shutdown only — per-turn cancellation uses the per-turn atomic (THR-003), never the stop token. | M1 | TSan/ASan on construction-destruction stress test; code review invariant |
| THR-013 | MUST | A Harness accepts any number of turns; accepted turns queue FIFO and exactly one HTTP transfer is active at a time (M1 baseline; evolution register governs multiplexing). | M1 | Integration test with N queued turns asserting order + single active transfer |
| THR-014 | MUST | A `send()` on a Conversation that already has a turn queued or in flight fails immediately with a distinct error category; the Conversation is untouched. | M1 | Unit test |
| THR-015 | MUST | Cancelling a still-queued turn removes it before any I/O is issued; its terminal event is Cancelled. | M1 | Machine + integration test |
| THR-016 | MUST | While the active turn awaits a main-thread tool result it retains the transfer slot; queued turns wait (documented M1 serialization). | M1 | Integration test |
| THR-017 | MUST | Harness destruction cancels all turns, aborts transfers, joins the worker within a bound set by transport-abort latency, and discards undelivered events; no callback fires after destruction begins. | M1 | Timed destruction stress test under TSan/ASan |
| THR-018 | MUST | The `update()` budget is a soft deadline checked between callbacks; an individual callback or tool handler is never preempted and may overrun it. | M1 | Unit test with deliberately slow callback |
| THR-019 | MUST | Callbacks may reentrantly call `send`, `cancel`, and tool registration (affecting subsequent turns only); reentrant `update()` is forbidden and diagnosed. | M1 | Reentrancy unit tests |
| THR-020 | MUST | An exception escaping a user callback propagates out of `update()`; the harness remains valid and the event counts as delivered. Callbacks SHOULD NOT throw. | M1 | Fault-injection unit test |
| THR-021 | MUST | Per-tool opt-in moves a handler to the worker thread (for thread-safe, slow handlers). | M4 | Thread-ID assertions per mode |

## Agentic Loop (SCRY-LOOP)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| LOOP-001 | MUST | The harness owns the full agentic loop (model → tool → result → resend, until final answer); the app never re-submits intermediate results. | M2 | Machine tests: multi-round tool sequences |
| LOOP-002 | MUST | The loop engine is sans-I/O: it performs no network, file, or clock access; it consumes events and emits commands only. | M2 | Code review invariant; machine test suite runs with no I/O linked |
| LOOP-003 | MUST | Loop rounds are bounded by configurable `max_tool_rounds`; exceeding it terminates the turn with a distinct error. | M2 | Machine test |
| LOOP-004 | MUST | Loop states are explicit (variant/enum) with a documented transition diagram; illegal transitions are unrepresentable or assert. | M2 | Exhaustive-transition machine tests |
| LOOP-005 | MUST | Time enters the machine only as injected events ("wake me at T"); retry backoff (exponential + jitter, configurable cap) is machine state. | M2 | Machine tests with fake clock |
| LOOP-006 | MUST | Retryability is decided by a pure classifier over error categories (429/5xx/transport retryable; auth/protocol not). | M1 | Table-driven unit test |
| LOOP-007 | SHOULD | Intermediate loop activity (tool calls, text deltas) is observable via optional callbacks without requiring app participation. | M2 | Integration test |
| LOOP-008 | MUST | Tool execution is at-most-once per tool-call ID, permanently: retry machinery never re-dispatches a tool that has been dispatched, regardless of transport failures. | M2 | Machine tests: failure injection around tool dispatch |

## Tool Registration (SCRY-TOOL)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| TOOL-001 | MUST | Explicit-schema registration (schema string + type-erased `json → json` callable) is public API and is the substrate all registration lowers onto. | M2 | Unit tests; reflection-OFF CI build uses only this path |
| TOOL-002 | MUST | The P2996 reflection layer derives schema and argument marshalling from plain aggregate structs at compile time; schemas are `constexpr` artifacts. | M3 | Compile-time tests; schema golden files |
| TOOL-003 | MUST | The reflection layer is severable: isolated header behind a feature macro; the library builds and passes tests with it disabled. | M0/M3 | CI matrix leg with reflection OFF |
| TOOL-004 | MUST | Members with default initializers become optional parameters; member names become parameter names. | M3 | Schema golden files |
| TOOL-005 | SHOULD | Reflected registration is concept-constrained; unsupported member types fail at the call site with a legible diagnostic. | M3 | Compile-fail tests |
| TOOL-006 | MUST | Tool-handler exceptions are caught at dispatch and returned to the model as tool-error results; they never propagate to the app. | M2 | Fault-injection unit tests |
| TOOL-007 | SHOULD | Parameter descriptions come from P3394 annotations when the toolchain supports them, else a customization point. | M3 | Schema golden files per toolchain |
| TOOL-008 | MAY | Tool return values may be any JSON-serializable type, including reflected structs. | M3 | Unit tests |
| TOOL-009 | MUST | Registering a duplicate tool name is rejected via `std::expected` error, not silent replacement; removal/replacement semantics are explicit API decisions deferred to M2 design. | M2 | Unit test |

## Provider & Protocol (SCRY-PROV)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| PROV-001 | MUST | An internal neutral message model (roles + content blocks incl. tool call/result) isolates all wire-format knowledge inside adapters. | M1 | Layering check: only adapters reference wire schemas |
| PROV-002 | MUST | Anthropic Messages adapter. | M1 | Golden-file round-trip tests |
| PROV-003 | MUST | OpenAI-compatible Chat Completions adapter (covers vLLM, Ollama, llama.cpp server, LM Studio). | M4 | Golden-file tests + e2e against local model |
| PROV-004 | MUST | Streaming (SSE) is supported on all adapters; the SSE parser is a pure incremental function tolerant of arbitrary chunk splits. | M1 | Property tests with randomized splits; fuzzing |
| PROV-005 | MUST | Adapter selection is config-driven (dialect enum + factory); no public plugin API until a concrete third-party need exists (evolution register). | M4 | Config-only switch test (API-004) |
| PROV-006 | SHOULD | Adapters are stateless translators; stream-parse state lives in per-turn parser objects. | M1 | Code review invariant; concurrent-turn test |
| PROV-007 | MUST | The neutral model carries multiple tool calls per assistant message with stable tool-call IDs, and accumulates partially-streamed JSON tool arguments before dispatch. | M2 | Golden-file + machine tests |
| PROV-008 | MUST | Unknown/unmappable *optional* stream events are skipped (debug-observable); unmappable *required* content (e.g., an unrecognized block the turn depends on) fails the turn with a protocol error — never silently discarded. | M1 | Adapter tests with synthetic unknown events |
| PROV-009 | SHOULD | Usage/token counts, finish reasons, and provider request IDs are surfaced on the completed turn. | M1 | Golden-file tests |

## Transport & Robustness (SCRY-NET)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| NET-001 | MUST | Transport sits behind an injectable seam; the full harness runs against a fake transport in tests. | M1 | Existence of integration suite on fake transport |
| NET-002 | MUST | All curl objects are RAII-wrapped; curl types are confined to implementation files; C-callback trampolines catch all exceptions. | M1 | Include audit; fault-injection under ASan |
| NET-003 | MUST | `stop_token` reaches curl progress callbacks so cancellation aborts transfers promptly (bounded latency). | M1 | Timed cancellation integration test |
| NET-004 | MUST | Malformed or hostile server output (broken SSE, invalid JSON, oversized payloads) must never crash or corrupt the host app: it becomes a protocol-category error. | M1 | Fuzzing (libFuzzer) on SSE + JSON boundaries; corpus in repo |
| NET-005 | MUST | Transient failures retry with exponential backoff + jitter, honoring `Retry-After`, under configurable max-attempt and elapsed-time caps (see LOOP-005/006). | M1 | Machine tests with fake clock |
| NET-006 | MUST | Retry eligibility: a request is retried only if no semantic output has been consumed (failure before the first content event). After partial output, the turn fails with a retryable-flagged error; the app decides. | M1 | Machine tests: failure injection at each stream stage |
| NET-007 | MUST | TLS certificate verification is on by default; disabling it is an explicit, named config option. | M1 | Integration test against mis-certified mock server |
| NET-008 | MUST | Resource bounds are configurable with documented defaults: max SSE event size, max response size, max tool argument/result size, and per-turn event-queue depth. Exceeding a bound fails the turn with a resource-category error; a stalled pump therefore bounds memory, not just event rate. | M1 | Unit tests at each bound |

## Errors (SCRY-ERR)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| ERR-001 | MUST | One error type end-to-end: category enum (auth, rate-limit, network, protocol, tool, cancelled) + message + provider detail. | M1 | Unit tests; no other error types cross the boundary |
| ERR-002 | MUST | Exactly one failure channel: the error event/callback. No status-polling API. | M1 | API surface review |
| ERR-003 | MUST | Every turn terminates in exactly one terminal event (complete, error, or cancelled) — never zero, never two. | M1 | Machine tests asserting terminal-event uniqueness across randomized event orders |
| ERR-004 | MUST | API keys and auth headers never appear in error messages, logs, or diagnostics — redacted at the transport boundary. Prompt/tool content is never logged by default. | M1 | Unit tests grepping error/log output under failure injection |
| ERR-005 | SHOULD | Errors and completed turns carry correlation identifiers (turn ID, attempt number, provider request ID where available). | M1 | Unit tests |

## Portability & Toolchain (SCRY-PORT)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| PORT-001 | MUST | Core library (reflection OFF) targets C++23 and builds on stable GCC and Clang. | M0 | CI matrix leg |
| PORT-002 | MUST | Reflection layer builds on GCC trunk and clang-p2996. | M0/M3 | CI matrix legs |
| PORT-003 | MUST | Runtime dependencies are limited to libcurl + Glaze; any addition requires a written justification committed with the change. | M0 | Dependency manifest review gate |
| PORT-004 | MUST | Glaze types do not appear in public headers; the tool-boundary JSON type is Scry-owned. | M2 | Include audit (API-002 machinery) |
| PORT-005 | MUST | Supported platforms: Linux and macOS from M0 (both reflection-ON and OFF legs). Windows is deferred to the evolution register. | M0 | CI matrix |
| PORT-006 | MUST | libcurl ≥ 7.84.0; `CURL_VERSION_THREADSAFE` is verified at first initialization (host threads may exist before the first Harness). | M1 | Startup check + version-pinned CI |
| PORT-007 | MUST | Pre-1.0: no API/ABI stability promises, breaking changes allowed with changelog notice. From 1.0: semver, inline-namespace ABI versioning. | M0 (documented) | Release checklist |

## Quality Gates (SCRY-QA) — binding form of ENGINEERING.md

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| QA-001 | MUST | Diff branch coverage ≥ 90% on new/changed lines; coverage exclusions require an inline justification. | M0 | Per-commit CI gate |
| QA-002 | MUST | Branch-coverage floor ≥ 95% on the sans-I/O machine, SSE parser, retry classifier, and schema generator. | M2+ | Per-component CI gate |
| QA-003 | MUST | No function on main with CRAP score > 30. | M0 | Per-commit CI gate + top-10 report |
| QA-004 | MUST | Cyclomatic complexity ≤ 15 per function (warn at 10); cognitive complexity ≤ 25. Named suppressions only. | M0 | lizard + clang-tidy gates |
| QA-005 | MUST | ASan, UBSan, and TSan suites pass per commit; threaded tests always run under TSan. | M0 | CI legs |
| QA-006 | MUST | Warnings-as-errors (`-Wall -Wextra -Wconversion -Wshadow`) across the full compiler matrix. | M0 | CI |
| QA-007 | MUST | All quality metrics ratchet: compared against main, they may hold or improve, never regress. | M0 | Ratchet comparison in CI |
| QA-008 | MUST | Unit/machine tests are deterministic: no real time, sleeps, or network. Flaky tests are fixed or deleted immediately. | M0 | Repeat-run CI check (e.g., 3× on suspicion) |
| QA-009 | MUST | Every bug fix lands with a regression test (machine-level replay where applicable). | M1+ | PR checklist gate |
| QA-010 | SHOULD | Nightly: mutation testing on machine/parsers, long fuzz runs, deep static analysis, e2e against a real local model. | M4+ | Nightly pipeline |
| QA-011 | SHOULD | Everything CI enforces is runnable locally with one command. | M0 | `just ci-fast` (or equivalent) exists and is documented |
| QA-012 | MUST | Definition of Done includes updating the four load-bearing docs — including this register — when behavior or a decision changes. | M0 | PR checklist gate |

## Unratified / Known Gaps

**None currently.** The four gaps surfaced by the original extraction audit were ratified during the pre-M0 architecture review (2026-07): platform targets → PORT-005, versioning/ABI → PORT-007, resource ceilings → NET-008, security posture → ERR-004/NET-007. New gaps land here, not in prose.

## Deferred to M2/M3 Design (tracked, not yet normative)

Per the pre-implementation review, these are deliberately not specified yet: complete C++-type→JSON-Schema mapping; defaulted vs. optional vs. nullable member semantics; schema dialect/subset, enum/nested-type behavior, unknown-field handling; tool registry replacement/removal/mutation rules beyond TOOL-009; detailed reflection diagnostics and annotation behavior (TOOL-005/007 set the bar, details at M3).
