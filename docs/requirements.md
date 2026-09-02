# Requirements

**This document is normative.** Where prose in the [design](design/overview.md), [architecture](architecture/overview.md), or [development](development/principles-and-testing.md) documentation conflicts with this register, the register wins; those documents provide rationale and context. Keywords MUST, MUST NOT, SHOULD, and MAY follow RFC 2119. Every requirement is enforced through one of the gate classes in the [Verification map](#verification-map): most mechanically — deterministic suites, the compiler matrix, sanitizer legs, package audits, or scheduled gates — and a named minority as review obligations discharged through the pull-request Definition of Done. A requirement with no credible enforcement path is a design smell and gets reworked, not waived.

ID scheme: `SCRY-<AREA>-NNN`, abbreviated to `<AREA>-NNN` in the tables below. IDs are permanent. A withdrawn requirement is moved to [Retired IDs](#retired-ids) with its reason and is never reused.

---

## API — Public Surface (SCRY-API)

| ID | Level | Requirement |
|---|---|---|
| API-001 | MUST | The public API is centered on five core concepts (Config, Conversation, ToolRegistry, Turn, Harness); no public type is designed for inheritance. |
| API-002 | MUST | No third-party types or headers (curl, Glaze) appear in the public include path. The four stateful handles (Conversation, ToolRegistry, Turn, Harness) use PImpl; configuration, errors, options, enums, event payloads, callback aggregates, and the Scry-owned JSON boundary type are plain value types. |
| API-003 | MUST | Scry-originated semantic and operational failures surface via `std::expected` before acceptance or through the turn's terminal callback after acceptance, never by throw. Allocation failure is excluded, and exceptions thrown by app callbacks propagate synchronously from `update()` per THR-020. |
| API-004 | MUST | Server/model configuration (base URL, auth, model, sampling params, reasoning mode, dialect) is a plain `Config` value aggregate; switching between Anthropic and the OpenAI-compatible common subset, including a local server with no API key, requires no code changes. Reasoning defaults to provider behavior; disabling it is supported only for the OpenAI-compatible dialect. |
| API-005 | MUST | The library never owns `main()`, never spins an event loop the app must join, and imposes no lifecycle on the host. |
| API-006 | MUST | Multiple Harness instances in one process work independently; no singleton or mutable global carries Harness/provider state. The sole process-wide runtime owner is an internal function-static libcurl RAII object whose immutable first-initialization result is shared by all Harnesses and whose successful lifetime ends at static teardown. Different configured dialects share the same isolation guarantee. |
| API-007 | SHOULD | Conversation history is serializable and deserializable for app-side persistence through `to_json()`/`from_json()`, which emit and strictly validate a canonical versioned Scry-owned document with lexicographically ordered object keys. **The document format is unstable before 1.0:** a document produced by one 0.x release is not guaranteed to be readable by another, and an announced canonicalization change may alter bytes while preserving JSON meaning. Applications MUST NOT treat it as a durable archival format across upgrades. |
| API-008 | MUST | A synchronous send-and-wait convenience exists, implemented on top of the async path (not a second code path). Because it is a pump loop, three consequences are documented on the operation: it drives `update()` until the waited turn terminates, so callbacks and app-thread tool handlers belonging to every other accepted turn run inside the call; the waited turn cannot be cancelled by the caller because no `Turn` handle is exposed, so it ends only through completion, a terminal error, or Harness destruction; and calling it from inside a callback is rejected with `invalid_state`. |
| API-009 | MUST NOT | The library does not provide prompt-template/chain DSLs and is not an inference engine. |
| API-010 | MUST | Fallible construction (Harness from Config and Conversation from its config) uses factories returning `std::expected`; semantic failures never throw. Allocation failure (`std::bad_alloc`) is excluded from the failure-as-value contract. |
| API-011 | MUST | Conversation commits are transactional: history is mutated only by the pump at terminal-event delivery. Success commits the full exchange — user message, every tool round, and the final answer — atomically; failure and cancellation commit nothing. |
| API-012 | MUST | Harness owns its ToolRegistry; there is no Conversation-local or process-global registry. `send()` snapshots registrations for the accepted turn, and registry changes affect subsequent turns only. |
| API-013 | MUST | Callback reference and view arguments are borrowed and remain valid only for the callback invocation; an app that retains them MUST copy them. This applies to the `std::string_view` passed to `on_text_delta` and the `const ToolCall&` passed to `on_tool_call`. `on_finished` instead receives its `Result<Completion>` by value, owned by that invocation. |
| API-014 | MUST | A read-only `JsonView` over the Scry-owned `Json` boundary type is public: value kind, scalar accessors, array index, object key lookup, and ordered key iteration in the same lexicographic order as the canonical form. Parsing produces one shared immutable document, so a child view outlives its parent; a default-constructed view reports `null` and every accessor returns empty. Parse failure is `invalid_argument`. `escape_json_string()` complements it for hosts that assemble small JSON results by hand. |
| API-015 | MUST | The neutral message model (`Role`, `TextBlock`, `ToolCallBlock`, `ToolResultBlock`, `ContentBlock`, `Message`) is public, and the internal model is that same family re-exported. `Conversation::messages()`, `system_prompt()`, and `busy()` expose committed state directly. The `messages()` reference is borrowed: it is valid until the next `Harness::update()` that commits a turn into that Conversation, or until the handle is moved or destroyed. A moved-from handle reports an empty history, an empty prompt, and not busy. |
| API-016 | MUST | Thin queries over state the runtime already holds are public: `Turn::finished()` (true once the terminal outcome was delivered, or once `update()` processed the terminal event when no `on_finished` was supplied; also true for a moved-from handle and when the Harness is gone), `Harness::cancel(TurnId)` (Turn::cancel addressed by id, true only for the call that issues the request), `ToolRegistry::contains()`/`names()` (registration order), and `static Harness::validate(const Config&)`, which runs exactly the checks `create()` runs on the configuration without initializing libcurl or starting a worker. `create()` may still fail afterwards for runtime reasons. |
| API-017 | MUST | The `on_tool_call` payload carries the canonical JSON result returned to the model and an `is_error` flag, which is true when the handler returned an error, threw, returned invalid JSON, or the model requested an unknown tool. The observer is skipped only when a framework failure fails the turn instead of producing a result. |

## Threading & Concurrency (SCRY-THR)

| ID | Level | Requirement |
|---|---|---|
| THR-001 | MUST | No async public API call blocks the calling thread on network I/O; all blocking work happens on a harness-owned worker thread. The explicitly named `send_and_wait` convenience is the sole blocking exception. |
| THR-002 | MUST | All user callbacks and all tool handlers execute only inside `update()`, on the thread calling it. |
| THR-003 | MUST | Mutable state crossing the thread boundary is limited to exactly three internally synchronized objects: command queue, event queue, and one atomic cancellation flag per turn. Accepted-turn commands MAY additionally share immutable history and tool-schema snapshot blocks with pump-owned state; the worker MUST NOT mutate those blocks, and pump-side mutation MUST first reseat any still-shared block copy-on-write. All other state is exclusively owned per the architecture runtime ownership table; the worker addresses turns only by immutable TurnId. |
| THR-004 | MUST | `update()` honors an optional caller-supplied time budget checked between events and callbacks; undelivered events roll to the next call, none are lost. Every call ingests at least one queued event and, when a deliverable callback exists and `max_callbacks` permits, delivers at least one callback before the budget is consulted, so a small or expired budget can never starve the pump. |
| THR-005 | MUST | Streaming deltas are coalesced (at most one aggregated text event per pump interval) so token rate cannot flood the queue. |
| THR-006 | MUST | `Turn::cancel()` is safe to call from the app thread at any time, including after completion; cancellation is cooperative and aborts in-flight transfers promptly. |
| THR-007 | MUST | Dropping a Turn handle detaches: the turn continues, the Conversation still commits on success, and the callbacks supplied to `send()` remain route-owned and deliverable. It never blocks or cancels implicitly; the handle controls only identity and cancellation. |
| THR-008 | MUST | Turn handles are move-only and expose exactly two operations, `id()` and `cancel()`. Callbacks are supplied to `send()`; the handle has no callback-registration surface. |
| THR-009 | MUST | `TurnCallbacks` MUST be attached infallibly and atomically as part of accepting `send()`, before the worker command becomes visible, so no event can precede the immutable callback set. There is no late registration or replay. An event with no matching callback is released immediately, while terminal processing still commits or rolls back the Conversation and clears its busy state. |
| THR-010 | MUST | Nothing throws across the thread boundary; worker-side failures become error events. |
| THR-011 | MUST | Tool handlers execute on the app thread inside `update()`. There is no worker-thread execution mode. |
| THR-012 | MUST | Worker lifetime uses `std::jthread`; the worker `stop_token` signals Harness shutdown only — per-turn cancellation uses the per-turn atomic (THR-003), never the stop token. |
| THR-013 | MUST | A Harness accepts up to `Config::limits.max_pending_turns`; accepted turns queue FIFO and exactly one HTTP transfer is active at a time (the evolution register governs multiplexing). Admission beyond the bound fails immediately with `resource_limit`. |
| THR-014 | MUST | A `send()` on a Conversation that already has a turn queued or in flight fails immediately with a distinct error category; the Conversation is untouched. |
| THR-015 | MUST | Cancelling a still-queued turn removes it before any I/O is issued; it terminates with an error carrying `ErrorCategory::cancelled`. |
| THR-016 | MUST | While the active turn awaits an app-thread tool result it retains the serialized turn slot; queued turns wait. |
| THR-017 | MUST | Harness destruction cancels all turns, aborts Scry-owned transfers, joins the worker within configured transport/shutdown bounds, and discards undelivered events; no callback or tool handler fires after destruction begins. Resolver behavior and every blocking curl phase MUST be covered by those bounds. |
| THR-018 | MUST | The `update()` budget is a soft deadline checked between callbacks; an individual callback or tool handler is never preempted and may overrun it. |
| THR-019 | MUST | Callbacks may reentrantly call `send`, `cancel`, and tool registration; such changes affect subsequently accepted turns only. Reentrant `update()` is forbidden: it performs no work, delivers no callbacks, and reports the rejection through `UpdateStats::budget_exhausted`. |
| THR-020 | MUST | An exception escaping a user callback propagates out of `update()`; the harness remains valid and the event counts as delivered. Callbacks SHOULD NOT throw. |

## Agentic Loop (SCRY-LOOP)

| ID | Level | Requirement |
|---|---|---|
| LOOP-001 | MUST | The harness owns the full agentic loop (model → tool → result → resend, until final answer); the app never re-submits intermediate results. |
| LOOP-002 | MUST | The loop engine is sans-I/O: it performs no network, file, or clock access; it consumes events and emits commands only. One machine carries the chat, retry, cancellation, and tool states. |
| LOOP-003 | MUST | Loop rounds are bounded by configurable `max_tool_rounds`; exceeding it terminates the turn with a distinct error. |
| LOOP-004 | MUST | Loop states are explicit (variant/enum) with a documented transition diagram in the architecture runtime documentation. Events that are illegal for the current state are diagnosed and MUST NOT mutate state or emit commands. |
| LOOP-005 | MUST | Time enters the machine only as injected events ("wake me at T"); retry backoff (exponential + jitter, configurable cap) is machine state. |
| LOOP-006 | MUST | Retryability is decided by a pure classifier over error categories (429/5xx/transport retryable; auth/protocol not). |
| LOOP-007 | SHOULD | Intermediate loop activity (text deltas, tool calls) is observable through the optional `TurnCallbacks::on_text_delta` and `on_tool_call` members supplied at `send()`, without requiring app participation; omitting them changes no loop behavior. |
| LOOP-008 | MUST | Within one turn, tool execution is at-most-once per tool-call ID: retry machinery never re-dispatches an ID already dispatched by that turn. A multi-call response is admitted to the event queue atomically, and a fatal framework dispatch or cumulative-result-budget failure suppresses every later handler in that batch before side effects. Failed/cancelled turns commit no tool rounds, so application-level resend of side-effecting tools requires an app-supplied idempotency or reconciliation policy. |

## Tool Registration (SCRY-TOOL)

| ID | Level | Requirement |
|---|---|---|
| TOOL-001 | MUST | Explicit-schema registration (schema string + type-erased `json → json` callable) is public API and is the substrate all registration lowers onto. Registrations are snapshotted per accepted turn, serialized to the provider, and dispatched from the pump. |
| TOOL-002 | MUST | The P2996 reflection layer derives a lexically canonical input schema and argument/return marshalling from plain aggregate structs. `scry::reflection::input_schema_v<Args>` is a `constexpr` artifact, and reflected registration lowers to TOOL-001 rather than creating a second registry or dispatch path. |
| TOOL-003 | MUST | The reflection layer is severable: it is a separate optional `reflection` package component and `scry::reflection` target behind `SCRY_ENABLE_REFLECTION`. The C++23 core builds, tests, installs, and is consumable with it disabled; core-only consumers receive no C++26 flags, reflection headers/runtime, Glaze headers/types, or exported Glaze target. |
| TOOL-004 | MUST | Reflected member names become parameter names. Presence and nullability are independent: only a default member initializer permits omission and removes a member from `required`; only `std::optional<T>` permits JSON `null`. Omission preserves the C++ initializer, and no JSON Schema `default` is emitted. |
| TOOL-005 | SHOULD | Reflected registration is concept-constrained. Root and nested objects are complete, default-initializable, non-union plain aggregates with no bases and public named non-bit-field writable members; unsupported roots, members, metadata, handlers, and returns fail at the call site with a stable, legible Scry diagnostic. |
| TOOL-006 | MUST | Tool-handler exceptions are caught at dispatch and returned to the model as tool-error results; they never propagate to the app. |
| TOOL-007 | MUST | Tool and parameter descriptions come from Scry's P3394 `[[=scry::reflection::description{"..."}]]` annotation and from no other source. Duplicate description annotations on one entity fail at compile time. There is no portable trait-based description fallback or override. |
| TOOL-008 | MAY | A reflected handler may return a direct TOOL-010 supported value or `Result` of one, including a reflected aggregate. The reflected overload does not accept `void`, `Status`, raw `Json`, references, futures, or awaitables; dynamic/unsupported returns use TOOL-001. |
| TOOL-009 | MUST | The registry is additive-only: registering a duplicate tool name is rejected via `std::expected`, not silent replacement, and there is no removal/replacement API. A future mutation surface requires an explicit snapshot/lifetime decision. |
| TOOL-010 | MUST | Supported reflected values are `bool`; non-character signed/unsigned integral types; finite `float`/`double`; `std::string`; scoped enums with unique underlying values encoded by exact enumerator name; one layer of `std::optional<T>`; `std::vector<T,Allocator>` except every `vector<bool,Allocator>` specialization; `std::array<T,N>`; and recursively supported plain aggregates. Integers carry exact C++ range bounds; fixed arrays carry exact length bounds. Nested optionals and enum aliases are rejected because their JSON decode/encode is ambiguous. Other C++ shapes are not inferred from Glaze support and remain unsupported until separately specified. |
| TOOL-011 | MUST | Generated schemas use Scry's closed provider-neutral JSON Schema 2020-12 subset: closed inline objects; the keywords `additionalProperties`, `anyOf`, `description`, `enum`, `items`, `maxItems`, `minItems`, `minimum`, `maximum`, `properties`, `required`, and `type`; minified JSON; lexicographically sorted object/property keys and `required` names; and enum declaration order. Generated schemas omit `$schema`, references/definitions, `title`, and `default`. This subset does not restrict explicit TOOL-001 schemas. |
| TOOL-012 | MUST | Reflected decoding is strict and recursive: it rejects a non-object root, unknown fields, missing required fields, wrong JSON kinds (including a lexical number such as `1.0` for an integral member), disallowed null, numeric sign/range/non-finite errors, unknown enum names, and fixed-array length errors. The canonical parsed JSON value is authoritative, so duplicate lexical object keys are not separately observable at dispatch. Decode failures become bounded model-visible tool errors; configured payload-limit failures remain fatal `resource_limit` errors. |
| TOOL-013 | MUST | `scry::reflection::add<Args>(ToolRegistry&, ToolMetadata, Handler&&)` invokes the handler with `std::move(args)`, preserves move-only captures, and lowers the typed wrapper and `input_schema_v<Args>` through the existing additive registry. |
| TOOL-015 | MUST | `scry::reflection::encode(const T&)` accepts every TOOL-010 supported value without tool registration and returns Scry-owned canonical `Json` through the same value encoder and error mapping used for reflected handler results. It remains confined to the optional reflection component under TOOL-003, and its bytes carry no cross-version archival guarantee. |

## Provider & Protocol (SCRY-PROV)

| ID | Level | Requirement |
|---|---|---|
| PROV-001 | MUST | An internal neutral message model (roles + content blocks incl. tool call/result) isolates provider wire-format mapping inside adapters. The generic JSON codec remains a provider-neutral bottom-layer facility. |
| PROV-002 | MUST | An Anthropic Messages adapter is supported. |
| PROV-003 | MUST | An OpenAI-compatible Chat Completions adapter implements the documented common subset for OpenAI, vLLM, Ollama, llama.cpp server, and LM Studio. This is a tested compatibility subset, not complete parity with every server extension or model/chat template. |
| PROV-004 | MUST | Streaming (SSE) is supported on all adapters; the SSE parser is a pure incremental function tolerant of arbitrary chunk splits. |
| PROV-005 | MUST | Adapter selection is config-driven (dialect enum + factory); no public plugin API until a concrete third-party need exists (evolution register). |
| PROV-006 | SHOULD | Adapters are stateless translators; stream-parse state lives in per-turn parser objects. |
| PROV-007 | MUST | The neutral model carries multiple tool calls per assistant message with stable tool-call IDs, and accumulates partially-streamed JSON tool arguments before dispatch. |
| PROV-008 | MUST | Unknown/unmappable *optional* stream events are skipped and represented by an internal debug-observable provider event; there is no public logging surface. Unmappable *required* content (e.g., an unrecognized block the turn depends on) fails the turn with a protocol error — never silently discarded. |
| PROV-009 | SHOULD | Usage/token counts, finish reasons, and provider request IDs are surfaced on the completed turn. |
| PROV-010 | MUST | OpenAI-compatible requests normalize an origin, `/v1` base, or full `/v1/chat/completions` endpoint; omit authorization for an empty key; use bearer auth for a safe nonempty key; encode system/text/function tools; and expand every neutral tool result into a separate ordered `role:"tool"` message. Sampling is validated per dialect. `ReasoningMode::provider_default` omits reasoning controls; `ReasoningMode::disabled` emits `reasoning_effort:"none"`. Request fixtures assert semantic JSON equivalence; the encoder currently emits lexicographically ordered object keys but does not promise exact wire bytes before 1.0. |
| PROV-011 | MUST | OpenAI-compatible streaming accepts arbitrary SSE chunk splits, accumulates bounded tool-call fragments by numeric index, requires contiguous complete function calls at finish, permits a trailing usage-only chunk, and emits exactly one completion only when `[DONE]` follows a finish reason. Missing/duplicate/early terminal markers and content after finish are protocol errors. |

## Transport & Robustness (SCRY-NET)

| ID | Level | Requirement |
|---|---|---|
| NET-001 | MUST | Transport sits behind an injectable seam; the full harness runs against a fake transport in tests. |
| NET-002 | MUST | All curl objects are RAII-wrapped; curl types are confined to implementation files; C-callback trampolines catch all exceptions. Process-global libcurl initialization is owned once until static teardown and is never churned with Harness lifetime. |
| NET-003 | MUST | Curl progress callbacks check both the worker `stop_token` (Harness shutdown) and the active turn's atomic flag (per-turn cancellation), so either aborts transfers promptly without conflating their scopes. |
| NET-004 | MUST | Malformed or hostile server output must never crash or corrupt the host app. Broken SSE, invalid JSON, and malformed or overflowing `Content-Length` response headers become `protocol` errors; a declared or received response above the configured bound becomes `resource_limit` per NET-008. |
| NET-005 | MUST | Transient failures retry with exponential backoff + jitter, honoring `Retry-After`, under configurable max-attempt and elapsed-time caps (see LOOP-005/006). Jitter is independently seeded per Harness without mutable process-global state; deterministic seed injection is confined to internal tests. Backoff and Retry-After scheduling are verified end to end against an injected worker clock; production always uses the steady clock. |
| NET-006 | MUST | Retry eligibility: a request is retried only if no semantic output has been consumed (failure before the first content event). After partial output, the turn fails with a retryable-flagged error; the app decides. |
| NET-007 | MUST | TLS certificate verification is on by default; disabling it is an explicit, named config option. A CA bundle path MAY be supplied for private or corporate CAs, which is the supported way to reach an internally signed endpoint without disabling verification. |
| NET-008 | MUST | Resource bounds are configurable with documented defaults: pending-turn count, SSE-event bytes, response bytes, tool argument/result bytes, per-turn queued-event bytes, and Conversation bytes. Exceeding admission limits rejects `send()`; exceeding an accepted turn's limit fails it with `resource_limit`. For tool turns, the remaining Conversation budget cumulatively covers assistant tool-call messages, every tool result, and the final answer before dispatch/resend/commit. A stalled pump therefore bounds memory, not just event rate. |
| NET-009 | MUST | Every transfer is bounded by a connect timeout, an idle timeout that fails the transfer when no response bytes arrive for `timeouts.idle`, and an optional total timeout `timeouts.transfer`. The idle bound is the default liveness guarantee for streaming responses; the total bound is unset by default. Both bounds fail the attempt with a retryable `network` error. |
| NET-010 | MUST | A proxy URL and verbatim extra request headers are plain `Config` fields validated at `Harness::create()` and `Harness::validate()`. Extra header names and values must pass the same header-injection validation as Scry's own headers, and a name that matches a Scry-managed header (`content-type`, `accept`, `authorization`, `x-api-key`, `anthropic-version`, case-insensitively) is rejected with `invalid_config`. The CA bundle path and proxy must contain no NUL, CR, or LF, and the proxy additionally no space or tab. Both reach libcurl as `CURLOPT_CAINFO` and `CURLOPT_PROXY`; empty values leave libcurl's defaults, including its `http_proxy` environment handling. |

## Errors (SCRY-ERR)

| ID | Level | Requirement |
|---|---|---|
| ERR-001 | MUST | One error type end-to-end: category enum (`invalid_config`, `invalid_state`, `invalid_argument`, `busy`, `authentication`, `rate_limit`, `network`, `protocol`, `resource_limit`, `tool`, `max_tool_rounds`, `cancelled`) plus message, sanitized provider detail, HTTP status, retryability/Retry-After metadata, and correlation fields. |
| ERR-002 | MUST | Immediate validation/admission/registration failure returns `std::expected`. After a turn is accepted, exactly one asynchronous outcome channel exists: `TurnCallbacks::on_finished`, which receives `Result<Completion>` — the completion on success or the `Error` on failure. Cancellation is delivered on that same channel as an `Error` carrying `ErrorCategory::cancelled`. There is no separate cancellation callback and no status-polling API. |
| ERR-003 | MUST | While its Harness remains alive, every accepted turn reaches exactly one terminal state — never zero, never two — and terminal processing commits or rolls back the Conversation even when `on_finished` is empty. A non-empty `on_finished` receives exactly one delivery. Harness destruction is the explicit exception: it aborts active work and discards all undelivered callbacks. |
| ERR-004 | MUST | API keys and auth headers never appear in error messages, logs, or diagnostics — redacted at the transport boundary. Prompt/tool content is never logged. Diagnostic logging is compile-time opt-in and writes only when `SCRY_LOG_FILE` names a nonempty explicit destination; unset or empty creates no default file. |
| ERR-005 | SHOULD | Errors and completed turns carry correlation identifiers (turn ID, attempt number, provider request ID where available). |
| ERR-006 | MUST | An error produced by a provider HTTP response carries the response status in `Error::http_status`. When the response body is a JSON object with an `error.type` or `error.code` string, the sanitized token reaches `provider_detail` under the dialect namespace; the body itself, the provider message, and every other field are never surfaced. |

## Portability & Toolchain (SCRY-PORT)

| ID | Level | Requirement |
|---|---|---|
| PORT-001 | MUST | Core library (reflection OFF) targets C++23 and builds on stable GCC/libstdc++ and Clang/libc++. |
| PORT-002 | MUST | The supported reflection component builds on GCC 16+ with `-std=c++26 -freflection` and a positive compiler-capability probe that compiles Scry's required P2996/P3394 annotation-query surface, including `annotations_of`, `is_annotation`, annotation-template identification, and payload extraction. clang-p2996 is deferred to manual, non-gating compatibility work: it is not a supported reflection configuration and MUST NOT produce installable or release artifacts. |
| PORT-003 | MUST | Runtime dependencies are limited to libcurl + Glaze; any addition requires a written justification committed with the change. |
| PORT-004 | MUST | Glaze headers and types do not appear in public headers; the tool-boundary JSON type and reflection JSON-view bridge are Scry-owned. |
| PORT-005 | MUST | The reflection-OFF C++23 core supports Linux and macOS. The reflection-ON GCC 16 component is supported on Linux first; macOS reflection support follows when a production-grade toolchain is practically distributable (evolution register row). Windows is deferred to the evolution register. |
| PORT-006 | MUST | libcurl ≥ 7.84.0; `CURL_VERSION_THREADSAFE` and asynchronous DNS are verified after the process's single initialization attempt (host threads may exist before the first Harness). The first result is cached. Capability failure cleans up immediately; successful initialization is cleaned up exactly once by the function-static owner's destructor at module/process teardown. |
| PORT-007 | MUST | Pre-1.0: no API/ABI stability promises. The Conversation persistence document format carries the same pre-1.0 instability (API-007). From 1.0: semver, inline-namespace ABI versioning. |

## Showcase Integrations (SCRY-SHOW)

| ID | Level | Requirement |
|---|---|---|
| SHOW-001 | MUST | An opt-in C++23 Dear ImGui chat-panel example consumes only the public `scry::scry` target and demonstrates non-blocking send, streamed text, terminal completion, error, and cancellation. The host owns and outlives the Harness and Conversation, calls `update()`, and owns the ImGui context, platform/renderer backends, window, and main loop. Panel destruction requests cancellation and MUST NOT block. |
| SHOW-002 | MUST | The NPC showcase is a deterministic, ephemeral 5-by-5 in-memory grid with explicit-schema zero-argument `look`, `move_north`, `move_south`, `move_east`, and `move_west` tools. The tools execute on the app thread, reject nonempty arguments, return canonical JSON, leave blocked boundary moves unchanged, and rely on application-owned idempotency or reconciliation for any durable adaptation. The live executable disables model reasoning through `Config`, reports executed tools, and fails rather than presenting a truncated, empty, or zero-tool completion as success. |
| SHOW-003 | MUST | The showcase adds no Scry public API, installed header, package target, export, or runtime dependency. Dear ImGui is a showcase-only MIT build dependency, pinned to `v1.92.8` commit `8936b58fe26e8c3da834b8f60b06511d537b4c63`, compiled only when `SCRY_BUILD_IMGUI_SHOWCASE=ON` (default `OFF`), and includes no window-system or renderer backend. A normal core build MUST NOT fetch or discover it. |
| SHOW-004 | MUST | The showcase gate builds with the repository warnings-as-errors policy and runs deterministic NPC tests, fake-controller panel behavior tests, a real Dear ImGui compile/link/headless-frame smoke, and the clean-package audit. It MUST be callable locally and by hosted CI. |

## Quality Gates (SCRY-QA) — binding form of the development documentation

| ID | Level | Requirement |
|---|---|---|
| QA-001 | MUST | New or changed behavior lands with tests at the sanctioned seam; a coverage exclusion requires an inline justification. Behavioral suites retained after the metric gates were retired remain while they cover live production behavior, but a test MAY be deleted with the implementation that was its only subject. |
| QA-002 | MUST | The sans-I/O machine, SSE parser, retry classifier, and reflection codec/bridge keep their near-total deterministic suites, including error paths. Type-directed constant-evaluation branches are covered by the compile-time positive/negative matrix, including the four reflection compile-fail diagnostics, and MUST NOT be represented by a misleading runtime percentage. |
| QA-004 | MUST | Cyclomatic complexity ≤ 15 per function (warn at 10); cognitive complexity ≤ 25. Named suppressions only. |
| QA-005 | MUST | ASan, UBSan, and TSan suites pass on every pull request; threaded tests always run under TSan. The reflection component's marshalling and Scry-owned JSON bridge run under ASan+UBSan on every reflection-affecting pull request (path-aware gate) and unconditionally in the scheduled ring — this rerun is the component's only sanitizer coverage, because the core sanitizer legs build with reflection OFF. |
| QA-006 | MUST | Warnings-as-errors (`-Wall -Wextra -Wconversion -Wshadow`) across the full compiler matrix. |
| QA-007 | MUST | Gates are behavioral: the compiler matrix with warnings-as-errors, the deterministic suites, sanitizers, clang-tidy, complexity limits, and the install/package-consumer audits gate every pull request. No gate scores a coverage or complexity-risk metric. |
| QA-008 | MUST | Unit/machine tests are deterministic: no real time, sleeps, or network. Flaky tests are fixed or deleted immediately. |
| QA-009 | MUST | Every bug fix lands with a regression test (machine-level replay where applicable). |
| QA-010 | SHOULD | The scheduled weekly ring runs deep static analysis (CodeQL), long fuzz runs on all protocol targets, the showcase gate, and the reflection component gate. The end-to-end smoke against a real local model runs on manual dispatch only, against a checksum-pinned server. |
| QA-011 | SHOULD | The per-commit CI ring is runnable locally with one command (`scripts/preflight.sh`), which reports any leg the host toolchain cannot provide. The scheduled-ring legs (CodeQL, long fuzz, showcase) and the manual local-model smoke (QA-010) are not part of the one-command ring; their local entry points are documented in the quality gates. |
| QA-012 | MUST | Definition of Done includes updating the relevant load-bearing documentation — including this register — when behavior or a decision changes. |
| QA-013 | MUST | Every exported public API declaration is documented, and the Doxygen HTML site builds without warnings on pull requests and every push to `main`. The documentation toolchain MUST remain build-only and outside installed/exported package metadata. |
| QA-014 | MUST | A pull request that claims a performance or memory improvement MUST provide rerunnable, compatible, same-host paired evidence for its head and immediate parent, preserve semantic output, and report representative guardrail scenarios. A stacked optimization additionally reports the affected cumulative scenarios against its profiling-foundation baseline. Measurements remain review evidence and MUST NOT become absolute shared-runner timing or memory gates without the evolution trigger; QA-007 remains authoritative for pull-request gates. |

## Verification map

Deliberately coarse: requirements map to gates and suites, not to individual
test titles, so the map survives suite refactors without churn. Pull-request
descriptions link the affected IDs (pull-request template), which is the
per-change end of this traceability. IDs called out as **review** have no
mechanical gate; they are discharged through the pull-request Definition of
Done and human review.

| Area | Mechanical enforcement | Review-enforced IDs |
|---|---|---|
| API | Public-header audit (`cmake/CheckPublicHeaders.cmake`), `public_api_contract` static assertions, runtime + integration suites, package-consumer audits | API-005, API-009 (design constraints) |
| THR | Runtime and integration suites on every pull request, repeated in full under TSan; shutdown/teardown bounds via the curl and loopback transport suites | — |
| LOOP | Deterministic sans-I/O machine suite (event-in/command-out replay, including retry and budget state) | — |
| TOOL | Registry and dispatch suites (TOOL-001/006/009); reflection schema/codec/bridge/registration suites plus the compile-fail matrix (TOOL-002/004/005/007/008/010–013/015); both package consumers for severability (TOOL-003) | — |
| PROV | Provider golden, stream, and edge suites; protocol fuzz targets in the scheduled ring | — |
| NET | Transport policy/curl/loopback suites and fake-transport integration suites; fuzz for hostile-input robustness; NET-002 additionally via clang-tidy and the sanitizer legs | — |
| ERR | Harness edge and integration suites, including the redaction assertions (ERR-004) | — |
| PORT | The compiler matrix itself (PORT-001), the reflection gate's configure-time probe (PORT-002), package audits (PORT-004), the curl capability check at first init (PORT-006) | PORT-003 (dependency justification), PORT-005 (platform policy), PORT-007 (release policy) |
| SHOW | Showcase gate (`ci-showcase.sh`): deterministic NPC and fake-panel suites, headless ImGui smoke, package-absence audit — weekly and on demand | — |
| QA | The CI workflows are themselves the gate (matrix, sanitizers, tidy, complexity flags, Doxygen); QA-010/QA-011 are properties of the workflow and script set; the profiling harness is built and smoke-run without timing thresholds | QA-001, QA-009, QA-012, QA-014 (review and habit clauses) |

## Retired IDs

Retired requirements are listed permanently so their IDs are never reused.

| ID | Retired | Reason |
|---|---|---|
| THR-021 | v0.1.0 | Opt-in worker-thread tool execution. Removed with `ToolExecution`/`ToolRegistrationOptions` in the v0.1.0 simplification; all tools execute on the app thread under THR-011. |
| TOOL-014 | v0.1.0 | Shared registration options across explicit and reflected registration. The option type it constrained no longer exists. |
| QA-003 | v0.1.0 | Per-function CRAP ceiling. Development-era gating machinery; complexity limits remain under QA-004 and untested-complexity risk is caught in review. |
