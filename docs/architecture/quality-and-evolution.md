# Architecture: Quality and Evolution

## 10. Testing & Tooling Practices

- **The test pyramid mirrors the architecture** — that is the point of the seams
  in the [runtime](runtime.md) and
  [provider/transport](tools-and-providers.md) architecture, and the
  [development testing plan](../development/principles-and-testing.md) lays out
  the resulting layers.
- Threaded code is tested under **TSan and ASan in CI** — sanitizers are cheap the day the code is written and impossible to retrofit onto a flaky foundation. UBSan on the reflection layer especially.
- Each provider dialect carries deterministic request/response/stream goldens,
  arbitrary-split stream coverage, a checked fuzz corpus, and a public Curl
  path/header/SSE case; cross-dialect integration proves two configured
  Harnesses cannot contaminate each other's decode state.
- **The package boundary is tested, not asserted.** The core matrix installs to
  a clean prefix, audits it for any reflection header, detail directory,
  library, or export, and builds a downstream `find_package(scry)` consumer.
  The reflection gate mirrors that for its component and adds the compiled
  proof of TOOL-003: a core-only C++23 consumer built with a non-reflection
  compiler against the same reflection-enabled installation.
  [Quality gates](../development/quality-gates.md) owns the ring shape and which
  leg runs where.
- **Warnings are errors** (`-Wall -Wextra -Wconversion`), from the first commit.
- **Diagnostic logging build:** `-DSCRY_ENABLE_LOGGING=ON` (preset `dev-logging`)
  compiles the internal `SCRY_LOG` macro into a small thread-safe file logger
  (`src/core/log.*`); every other build compiles the macro to nothing. Even in
  a logging build, output is off until the `SCRY_LOG_FILE` environment variable
  names a destination — Scry never writes a log file the host did not ask for.
  When enabled it appends timestamped lifecycle lines: turn
  start/completion/failure, model-request attempts and retries, tool dispatch,
  and ignored provider stream events. Lines carry only turn ids, tool names,
  attempt counts, and error categories; prompt/tool content and credentials
  never reach the log (ERR-004), and this remains an internal diagnostic, not
  the public logging surface PROV-008 notes is absent.

## 11. Evolution Register: Deliberate Simplifications and Their End States

Every "boring first" choice is recorded here with the condition that triggers evolution and the intended destination — so simplicity stays a decision, not an accident. Additions to this codebase that simplify deliberately must add a row.

| Simplification (now) | Trigger to evolve | Desired end state |
|---|---|---|
| Mutex + deque + condvar queues | Profiling shows queue contention or pump latency in a real app | Lock-free MPSC (commands) / SPSC (events) behind the same interface; interface designed for this swap from day one |
| One worker thread per Harness, one turn in flight per Conversation | A real app needs concurrent turns at scale | curl-multi–driven multiplexing of N turns on one worker; the actor + sans-I/O split means the machine layer is untouched |
| Blocking `send`-and-wait built on pump-until-complete | Coroutine-scheduler apps appear as users | `co_await`-able turn awaitable layered on the event queue; core remains callback/pump-based |
| Provider factory keyed on internal enum, no plugin API | A third-party provider that can't be upstreamed | Public adapter concept + registration hook; only then |
| OpenAI-compatible common Chat Completions subset only | A supported deployment requires Azure, Responses API, structured output, or another extension | Ratify a separate adapter/contract; never grow compatibility by wire-format guessing |
| Closed reflected-value matrix; P3394 annotations are the only description source | A concrete tool needs an unsupported type/constraint, or a description that cannot be an annotation | Add one schema/decode/encode/diagnostic contract at a time; ratify a second description source only with an explicit precedence rule |
| No connection pooling beyond curl defaults | Measured connect/TLS overhead in streaming-heavy use | curl share/multi connection reuse, invisible above the transport seam |
| Scry-owned `UniqueFunction` at callable boundaries | All supported macOS/Linux standard libraries ship a conforming `std::move_only_function` and a pre-1.0 API change is acceptable | Replace the small owned erasure with the standard facility after ABI and allocation benchmarks |
| Additive-only ToolRegistry with immutable accepted-turn snapshots | A concrete hot-reload or dynamic-plugin use case needs mutation | Explicit replace/remove operations with documented snapshot and handler-lifetime semantics |
| Linux + macOS only | Concrete Windows user demand | Windows reflection-OFF via clang; MSVC leg only if/when P2996 ships there |
| Reflection-ON CI leg on Linux only (PORT-005) | A production-grade P2996 toolchain becomes practically distributable on macOS | Gating reflection legs on both platforms |
| Serialized turns: queued turns wait while the active turn awaits a tool result | Serialized scheduling measurably limits a real app | Tool-await releases the slot under curl-multi multiplexing (same trigger as row 2) |
| All tool handlers run synchronously on the app thread inside `update()` | A real tool is slow enough that running it on the app thread costs the host its frame budget | An asynchronous/deferred tool-result API: the handler accepts the call, returns immediately, and completes the result later through the pump — with its own cancellation, ordering, and budget rules ratified up front |
| Showcase ImGui panel has no platform/renderer backend and the NPC world is ephemeral | A maintained standalone demo or durable game integration becomes a real deliverable | Ratify its platform matrix and lifecycle separately; keep any backend, persistence, rollback, or idempotency machinery outside the Scry package |
| Streaming-only provider seam: adapters always request `stream: true` and decode through the stream path; the parallel non-streaming response decoders were removed as production-dead | A supported deployment genuinely cannot serve SSE, or a consumer needs non-streaming completions | Reintroduce a `parse_response` seam together with a runtime mode that actually exercises it, plus its golden and fuzz coverage — never as untested parallel code |
| Compile-time diagnostic file logger (`SCRY_ENABLE_LOGGING`): fixed line format, one file sink chosen by environment variable, no runtime configuration or public API | A consumer needs runtime-toggleable, structured, or callback-driven diagnostics | Ratify a public logging/observer surface (the one PROV-008 records as absent) and route the same call sites through it |
| Release-posture verification uses behavioral gates only — matrix, tests, sanitizers, tidy, package audits; no coverage/CRAP metric gating, no mutation testing, fuzz and showcase on the scheduled ring | Unattended agent-driven development resumes at scale, or coverage erosion on the pure components is observed in review | Restore targeted pieces, starting with a single non-gating coverage report line, never the full retired apparatus by default |
| Reflection CI gating is path-aware: the GCC 16 leg — with its ASan+UBSan rerun, the component's only sanitizer coverage — gates reflection-affecting pull requests and runs unconditionally in the weekly scheduled ring | A reflection regression merges through a gap in the path filter, or the experimental toolchain stabilizes into a plain distribution package | Widen the path filter first; return the leg to the unconditional pull-request ring only if filtering itself proves unsound |
| v0.1.0 runtime simplification: worker-thread tool execution and its registration options removed, turn callbacks supplied once at `send()` with a single terminal outcome, one JSON codec with one canonical form | A slow tool, a coroutine host, or a second serialization shape presents a concrete need the simplified surface cannot express | Reintroduce capability by contract, not by restoring the old machinery: an async/deferred tool-result API for slow tools (row above), a `co_await`-able turn for coroutine hosts, and a ratified second document contract before any second canonical form |
| `ToolHandler` takes `Json` by value | Tool-argument copies show up on the frame thread inside `update()`, and a pre-1.0 API break is acceptable | `UniqueFunction<Result<Json>(const Json&)>`; keep `Json` an opaque UTF-8 string |

## 12. Pattern Summary

| Piece | Governing pattern / idiom |
|---|---|
| Public types | PImpl handles, plain contract values, Rule of Zero |
| Concurrency | Actor model; message passing over variant commands/events; jthread + stop_token |
| Delivery | Single-threaded-by-construction pump with time budget |
| In-flight turns | Move-only PImpl handle (`id()`/`cancel()`) + shared cancel flag + weak pump route; callbacks owned by the turn from `send()` |
| Agentic loop | Sans-I/O explicit state machine; time as injected events |
| Tool registry | One type-erased registration substrate, app-thread dispatch; optional consteval codegen and strict Scry-owned JSON bridge above it |
| Providers | Config-selected Strategy at a narrow seam; stateless adapters with per-turn dialect state and golden-file tests |
| Transport | RAII curl, C-callback trampolines, injectable seam; pure incremental SSE parser |
| Errors | One categorized value type; expected before acceptance, one async error event after |
| Showcases | Outermost application adapters over public `scry::scry`; host-owned lifecycle and state; no install/export path |
| Extensibility | Callables and config, not inheritance; YAGNI on plugin machinery |
