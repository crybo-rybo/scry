# Scry — Requirements Register

**This document is normative.** Where prose in [DESIGN.md](DESIGN.md), [ARCHITECTURE.md](ARCHITECTURE.md), or [ENGINEERING.md](ENGINEERING.md) conflicts with this register, the register wins; the other documents provide rationale and context. Keywords MUST, MUST NOT, SHOULD, and MAY follow RFC 2119. Every requirement carries the milestone it lands in (per DESIGN.md §12) and how it is verified — "planned" until the verifying test or gate exists, at which point the row is updated to name it. A requirement with no credible verification path is a design smell and gets reworked, not waived.

ID scheme: `SCRY-<AREA>-NNN`. IDs are permanent; withdrawn requirements are struck, never reused.

---

## API — Public Surface (SCRY-API)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| API-001 | MUST | The public API surface is limited to a small set of value/handle types (Client, Conversation, ToolRegistry, Turn, Harness); no public type is designed for inheritance. | M0 | Header review; no virtual/protected in public headers (grep gate) |
| API-002 | MUST | All public types use PImpl; no third-party headers (curl, Glaze) appear in the public include path. | M0 | IWYU + include-graph CI check |
| API-003 | MUST | The public boundary is exception-free: failures surface via `std::expected` or the error callback, never by throw. | M1 | API tests exercising all failure paths; no `throw` reaches boundary under ASan/UBSan suites |
| API-004 | MUST | Server/model configuration (base URL, auth, model, sampling params, dialect) is declarative config on Client; switching providers or local servers requires no code changes. | M1/M4 | Integration test: same app code against two dialects via config only |
| API-005 | MUST | The library never owns `main()`, never spins an event loop the app must join, and imposes no lifecycle on the host. | M0 | Design invariant; example apps demonstrate integration into pre-existing loops |
| API-006 | MUST | Multiple Harness instances in one process (including different providers) work independently; no singletons or mutable globals beyond the ref-counted curl-global guard. | M1 | Integration test with two concurrent Harnesses; symbol audit |
| API-007 | SHOULD | Conversation history is serializable/deserializable for app-side persistence. | M2 | Round-trip unit test |
| API-008 | MUST | A synchronous send-and-wait convenience exists, implemented on top of the async path (not a second code path). | M1 | Unit test; code review invariant |
| API-009 | MUST NOT | The library does not provide prompt-template/chain DSLs and is not an inference engine. | — | Scope gate at review |

## Threading & Concurrency (SCRY-THR)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| THR-001 | MUST | No public API call blocks the calling thread on network I/O; all blocking work happens on a harness-owned worker thread. | M1 | Integration test with watchdog timing on API calls |
| THR-002 | MUST | All user callbacks (turn events and default-mode tool handlers) execute only inside `update()`, on the thread calling it. | M1 | Thread-ID assertions in integration tests; TSan suite |
| THR-003 | MUST | State crosses the thread boundary only via the command/event queues; no other shared mutable state. | M1 | TSan on all threaded tests, per commit |
| THR-004 | MUST | `update()` honors an optional caller-supplied time budget; undelivered events roll to the next call, none are lost. | M1 | Unit test with fake clock + event counting |
| THR-005 | MUST | Streaming deltas are coalesced (at most one aggregated text event per pump interval) so token rate cannot flood the queue. | M1 | Unit test with high-rate fake stream |
| THR-006 | MUST | `Turn::cancel()` is safe to call from the app thread at any time, including after completion; cancellation is cooperative and aborts in-flight transfers promptly. | M1 | Integration test: cancel at randomized points (incl. mid-stream, mid-tool) under TSan |
| THR-007 | MUST | Dropping a Turn handle detaches (turn continues, events still deliverable); it never blocks or cancels implicitly. | M1 | Unit test; destructor audit |
| THR-008 | MUST | Turn handles are move-only. | M0 | `static_assert` in tests |
| THR-009 | MUST | Callbacks attached after events have begun arriving receive buffered prior events; no events are raced past or dropped. | M1 | Unit test attaching late at randomized points |
| THR-010 | MUST | Nothing throws across the thread boundary; worker-side failures become error events. | M1 | Failure-injection tests under sanitizers |
| THR-011 | MUST | Tool handlers execute on the app thread inside `update()` by default; per-tool opt-in moves a handler to the worker thread. | M2 | Thread-ID assertions per mode |
| THR-012 | MUST | Worker lifetime uses `std::jthread`/`std::stop_token`; harness destruction joins cleanly with no leaked threads. | M1 | TSan/ASan on construction-destruction stress test |

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

## Provider & Protocol (SCRY-PROV)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| PROV-001 | MUST | An internal neutral message model (roles + content blocks incl. tool call/result) isolates all wire-format knowledge inside adapters. | M1 | Layering check: only adapters reference wire schemas |
| PROV-002 | MUST | Anthropic Messages adapter. | M1 | Golden-file round-trip tests |
| PROV-003 | MUST | OpenAI-compatible Chat Completions adapter (covers vLLM, Ollama, llama.cpp server, LM Studio). | M4 | Golden-file tests + e2e against local model |
| PROV-004 | MUST | Streaming (SSE) is supported on all adapters; the SSE parser is a pure incremental function tolerant of arbitrary chunk splits. | M1 | Property tests with randomized splits; fuzzing |
| PROV-005 | MUST | Adapter selection is config-driven (dialect enum + factory); no public plugin API until a concrete third-party need exists (evolution register). | M4 | Config-only switch test (API-004) |
| PROV-006 | SHOULD | Adapters are stateless translators; stream-parse state lives in per-turn parser objects. | M1 | Code review invariant; concurrent-turn test |

## Transport & Robustness (SCRY-NET)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| NET-001 | MUST | Transport sits behind an injectable seam; the full harness runs against a fake transport in tests. | M1 | Existence of integration suite on fake transport |
| NET-002 | MUST | All curl objects are RAII-wrapped; curl types are confined to implementation files; C-callback trampolines catch all exceptions. | M1 | Include audit; fault-injection under ASan |
| NET-003 | MUST | `stop_token` reaches curl progress callbacks so cancellation aborts transfers promptly (bounded latency). | M1 | Timed cancellation integration test |
| NET-004 | MUST | Malformed or hostile server output (broken SSE, invalid JSON, oversized payloads) must never crash or corrupt the host app: it becomes a protocol-category error. | M1 | Fuzzing (libFuzzer) on SSE + JSON boundaries; corpus in repo |
| NET-005 | MUST | Transient failures retry with exponential backoff + jitter under a configurable cap (see LOOP-005/006). | M1 | Machine tests with fake clock |

## Errors (SCRY-ERR)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| ERR-001 | MUST | One error type end-to-end: category enum (auth, rate-limit, network, protocol, tool, cancelled) + message + provider detail. | M1 | Unit tests; no other error types cross the boundary |
| ERR-002 | MUST | Exactly one failure channel: the error event/callback. No status-polling API. | M1 | API surface review |
| ERR-003 | MUST | Every turn terminates in exactly one terminal event (complete, error, or cancelled) — never zero, never two. | M1 | Machine tests asserting terminal-event uniqueness across randomized event orders |

## Portability & Toolchain (SCRY-PORT)

| ID | Level | Requirement | Milestone | Verification (planned) |
|---|---|---|---|---|
| PORT-001 | MUST | Core library (reflection OFF) targets C++23 and builds on stable GCC and Clang. | M0 | CI matrix leg |
| PORT-002 | MUST | Reflection layer builds on GCC trunk and clang-p2996. | M0/M3 | CI matrix legs |
| PORT-003 | MUST | Runtime dependencies are limited to libcurl + Glaze; any addition requires a written justification committed with the change. | M0 | Dependency manifest review gate |
| PORT-004 | MUST | Glaze types do not appear in public headers; the tool-boundary JSON type is Scry-owned. | M2 | Include audit (API-002 machinery) |

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

Surfaced by the extraction audit; tracked here until promoted to numbered requirements or explicitly rejected:

- **Platform targets.** Linux is implied by the toolchains; macOS/Windows support is nowhere stated. Needs a decision (curl and the design are portable; MSVC affects reflection only — a MinGW/clang-on-Windows reflection-OFF build is plausible).
- **Versioning & ABI policy.** PImpl buys ABI stability but no semver/inline-namespace policy is written down. Decide before first tagged release.
- **Resource ceilings.** No stated bounds on memory (event queue depth under a stalled pump, conversation size) — the backpressure story covers rate, not accumulation. Worth a THR/NET requirement before M1 hardening.
- **Security posture.** API keys are config; nothing states redaction rules for logs/errors. Should become an ERR/NET requirement.
