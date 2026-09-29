# Review: Scry over pluggable backends

Reviews `docs/proposals/backends.md` (commit 4820767) against the current tree of
Scry (branch `claude/scry-backend-architecture-md2qjh`) and llamad (`main`,
80deb60). All citations are `file:line` in the named repository. Scry paths are
relative to `/home/user/scry`; llamad paths are prefixed `llamad/`.

## Verdict

**Amend the plan; do not replace it.** The seam is real, it is at the right
level, and the conservative shape (the worker, queues, pump, Conversation, and
registry stay; `perform()` blocks on the worker) is the right one for two
backends. None of the radical alternatives buys enough to pay for the
guarantees it would reopen. Several things in the proposal need fixing before
Phase 1, though, and two of them would ship bugs:

1. **The error contract as written cannot be implemented.** The machine throws
   away the backend's `retryable` flag and recomputes it from the category
   (`src/machine/turn_machine.cpp:474`, `src/core/retry.cpp:55-57`). The llamad
   mapping's "`Error{network}`, not retryable" row is impossible: `network` is
   always retried.
2. **Any `cancelled` error from a backend cancels the turn**
   (`src/machine/turn_machine.cpp:183-185`). The mapping sends gRPC `CANCELLED`
   to `Error{cancelled}` without checking that the host asked for it, so a
   server-side cancel would show up to the host as a cancel it never made.
3. **The proposal misses HTTP dependencies in the runtime:** API-key redaction
   in the worker, all of `config.cpp`, the libcurl link, and the curl-derived
   jitter seed.
4. **Phase 0 cannot both switch to `std::stop_token` and leave every test
   unchanged.** The cancel flag is part of the `Transport` signature that five
   test fakes override, and some tests read it directly.
5. **Phase 3 is a port, not a lowering.** llamad's function reflection
   disagrees with Scry's on optional parameters, unknown keys, `std::string`
   results, exception text, and where descriptions come from.
6. **llamad #18 is not a prerequisite for a shim built inside llamad.** The
   real prerequisite is a Scry runtime target that does not require libcurl.

## Job 1: Claims checked against the code

### "What the code already says" (proposal lines 33-70)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1.1 | `perform_attempt()` composes `make_request`, `Transport::perform`, `SseParser::push`, `parse_stream_event` | **Confirmed** | `src/runtime/worker.cpp:256-281`, `:283-307`; `src/core/transport.hpp:43-45`; `src/core/provider.hpp:67-68` |
| 1.2 | The machine consumes exactly `ModelTextDelta`, `ModelSemanticOutput`, `ModelCompleted`, `AttemptFailed` from an attempt | **Confirmed**, with a caveat | `src/machine/turn_machine.hpp:35-50`. `AttemptFailed` also carries `observed_at` and `jitter_sample`, which the worker fills (`worker.cpp:244-249`). The caveat: the machine reinterprets two fields of the `Error` the backend hands over (rows 1.3, 1.4). |
| 1.3 | "Each backend supplies `category`, `retryable`, `retry_after`, ..." | **Incorrect** | `correlate()` overwrites `retryable = is_retryable(category)` (`turn_machine.cpp:473-478`). Retry eligibility is purely by category: only `network` and `rate_limit` qualify (`retry.cpp:55-57`, `turn_machine.cpp:187`). `retry_after` is honoured (`worker.cpp:237`). Transport code sets `retryable` today (`curl_error.cpp:51,53`), but the machine overwrites it anyway. |
| 1.4 | (implicit) a backend may return `cancelled` for gRPC `CANCELLED` | **Incorrect / Missed** | `AttemptFailed` with `category == cancelled` becomes `CancelTurn` (`turn_machine.cpp:183-185`), so the host gets `on_finished(cancelled)` for a cancel it never made. HTTP never hits this because curl only returns `cancelled` when the stop or flag was actually set (`curl_transport.cpp:144-153`, `curl_error.cpp:33-35`). |
| 1.5 | `ModelRequest` is system prompt, history snapshot, turn messages, tool snapshot, sampling; `ModelResponse` is content, finish reason, usage, request id | **Confirmed** | `src/core/model.hpp:35-50`, `:55-60` |
| 1.6 | "Everything above that point is backend-neutral" | **Partly** | Pump and turn route have no HTTP dependency (grep of `src/runtime/pump.cpp`, `turn_route.cpp`: none). The runtime still depends on HTTP in the places listed in 1.7. |
| 1.7 | Runtime dependencies the proposal missed | **Missed** | **Worker:** redacts `config_.api_key` from attempt errors (`worker.cpp:61-76`, `:235`) and from completion request ids (`:332-334`); falls back to the transport's request id (`:329-331`); sizes the SSE parser from `limits.max_sse_event_bytes` (`:104`); includes `protocol/sse.hpp` (`:4`, `worker.hpp:6`). **Config:** `config.cpp` validates URL and model (`:46-54`), auth by dialect (`:56-64`), reasoning by dialect (`:66-78`), sampling by dialect (`:80-135`), timeouts through `transport_policy::validate_timeouts` (`:149`), SSE and response limits in `positive_limits` (`:22-28`), and headers, proxy, and CA bundle (`:178-196`); it includes `transport/transport_policy.hpp` (`:3`). **Harness:** builds the adapter and `CurlTransport` and checks curl status (`harness.cpp:209-213`); seeds jitter from the transport pointer (`:214`); `HarnessTestAccess::create` takes adapter + transport (`harness.cpp:307-324`, `test_access.hpp:21-24`). **Testing:** `create_harness` calls `make_provider_adapter(config.dialect)` (`testing/src/scripted_transport.cpp:239-247`). **Build:** `find_package(CURL 7.84 REQUIRED)` and a private `CURL::libcurl` link on `scry` (`CMakeLists.txt:73`, `:166`). `startup.hpp` is backend-neutral (`startup.hpp:10-23`). `pump.cpp` has no dependency. |
| 1.8 | "Terminal events always fit" still holds with a foreign backend | **Missed** | `bound_terminal_event` trims oversized errors, but its comment relies on "Transport policy caps a real identifier well under the reserve" (`worker.cpp:45-48`). Under a backend interface, sanitizing and bounding `provider_detail` and `request_id` becomes a backend promise, while `error.hpp:42-43` promises hosts they are always sanitized. The runtime should enforce it rather than trust each backend. |
| 1.9 | llamad boundary 4 matches Scry's snapshots | **Confirmed** | `llamad/AGENTS.md:70-78`; Scry snapshots in `model.hpp:30-33`, `harness.cpp:121,137-143` |
| 1.10 | The #18 quotation | **Confirmed** as a quote | Issue #18's "Where this matters" section. Its substance differs from how the proposal uses it; see 5.x. |

### The interface (proposal lines 72-150)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 2.1 | `text()` / `semantic_output()` can reproduce "retry only before semantic output" | **Confirmed, if the contract is tightened** | The machine enters `StreamingState` on either event (`turn_machine.cpp:130`, `:142`) and refuses retry once there (`:187`). So order is irrelevant: `ModelSemanticOutput` need not come before the first text delta. Today the worker raises it once per decoded batch, before that batch's deltas (`worker.cpp:414-416`), from a flag the decoders set sticky (`provider.hpp:58`). What matters is that it is raised **before the attempt returns an error** for any content that did not arrive as text. |
| 2.2 | Where the HTTP decoders set semantic output | Detail the interface must keep | Anthropic sets it on every content-block start, **including an empty text block that never streams a delta** (`src/provider/anthropic_stream.cpp:35-43`), on text deltas (`:176`), and on tool JSON deltas (`:201`). OpenAI sets it on tool-call fragments (`openai_stream.cpp:226`) and non-empty text (`:309`), but not on empty text (`:290-296`). `HttpBackend` must call `semantic_output()` on every flag flip, not only on text. |
| 2.3 | "An empty chunk (issue #19 progress) is consumed silently" | **Confirmed as necessary; the interface must say so** | If `text("")` implied semantic output, a #19 prefill heartbeat would switch off retry before the model had said anything. The header should state that `text("")` is ignored and implies nothing. |
| 2.4 | Final text and streamed text agree | **Missed** | Nothing checks that streamed deltas and the returned `content` agree. The HTTP decoders agree by construction (`anthropic_stream.cpp:176-181`, `openai_stream.cpp:308-310`). `Completion::text` comes from the content; history is what gets committed. The conformance suite should assert that the concatenated deltas equal the concatenated text blocks. |
| 2.5 | Borrowed `BackendRequest` "for the duration of perform()" | **Partly** | Today the worker drops the request snapshot right after encoding, so that a completion moves the transcript instead of copying it (`worker.cpp:252-261`, `turn_machine.cpp:288-295`). With a borrowed view, the worker holds the snapshot until `perform()` returns. That is still copy-free, but only if the worker releases it **before** applying `ModelCompleted` (`turn_machine.cpp:375`). State this ordering in the worker. |
| 2.6 | `BackendRequest` carries everything a backend needs | **Partly** | `max_tool_arguments_bytes` is needed on both sides: by the HTTP decoder while it streams (`worker.cpp:105`, `anthropic_stream.cpp:195-200`) and by the machine at completion (`turn_machine.cpp:61-65`). Add it to `BackendRequest`, or have `http::Config` duplicate it. |
| 2.7 | `ContentBlock`, `Message`, `ToolDefinition`, `FinishReason`, `Usage` are public | **Confirmed** | `include/scry/message.hpp:12-59`, `tool_registry.hpp:21-28`, `events.hpp:21-45` |
| 2.8 | Public headers leak HTTP | **Missed** | `Error::http_status` (`error.hpp:55-57`); `HttpHeader`, `ProviderDialect`, `ReasoningMode`, `TransportTimeouts` in `config.hpp`; `Harness::create` and `validate` docs name libcurl (`harness.hpp:37-48`). The `ErrorCategory` docs say "Provider" throughout (`error.hpp:22-29`). |

### Cancellation (proposal lines 142-147, 329-331)

How the flag flows today:

- It is created on the **host** in `send()` (`harness.cpp:122`) and shared by
  the `TurnRoute` (`:128-129`), the `SendTurnCommand` (`:150`,
  `worker_messages.hpp:21`), and `Turn::Impl` (`turn_impl.hpp:13,23`).
- `Turn::cancel()` goes through the route, or flips the flag directly once the
  route is gone (`turn.cpp:25-33`). `TurnRoute::cancel()` refuses after
  terminal, then does `exchange(true)` and pushes a `CancelTurnCommand`
  (`turn_route.cpp:104-115`).
- The **host** reads it before dispatch (`turn_route.cpp:264`), after admission
  (`:249`), and after the handler (`:288`).
- The worker reads it at turn start (`worker.cpp:175`), before each request
  (`:202`), in retry wait (`:343,352`), and in tool wait (`:364`). It also
  handles `CancelTurnCommand` for queued turns (`:153-158`) and during tool
  wait (`:395-398`).
- curl polls it in `validate_execution` (`curl_transport.cpp:274`) and in the
  progress callback and driver loop (`:144-159`, `:286`). The poll wait is
  bounded by `timeouts.shutdown` (`:297`). The fakes poll every 2 ms
  (`testing/src/scripted_transport.cpp:176-183`).

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 3.1 | "The worker owns one `std::stop_source` per turn" | **Incorrect owner** | The host must create it in `send()`: the route and `Turn` read and request it before the worker ever sees the turn, and `Turn::cancel()` must still work after the Harness is gone (`turn.cpp:32`). |
| 3.2 | A token preserves every guarantee in "Threading and lifetime" | **Confirmed, with the obligations below** | Cancel before start: `worker.cpp:175`, `:202`, `:153-158` survive as `stop_requested()`. Cancel inside `on_tool_request`, and the result suppressed after a tool returns: the host-side reads (`turn_route.cpp:249,264,288`) become `stop_requested()`. `request_stop()`'s return value matches `exchange`'s "first caller wins" (`turn_route.cpp:108`). Destruction: `~Impl` requests stop and joins (`harness.cpp:80-86`); promptness becomes the backend's job. |
| 3.3 | Subtlety: `stop_callback` runs on the cancelling thread | **Missed** | `request_stop()` runs registered callbacks synchronously on the host, inside `Turn::cancel()` / `Harness::cancel()` (both `noexcept`), possibly inside `update()` from a callback, and inside `~Harness` if shutdown is chained into the turn's source. Backend callbacks therefore must be `noexcept` (a throw inside a stop callback calls `std::terminate`), non-blocking, and thread-safe. A `~stop_callback` on the worker blocks while the host is running that callback, so a callback that takes a lock the worker holds while tearing down deadlocks. gRPC `TryCancel` meets all of this (`llamad/client/src/client.cpp:51-70`). curl can only use `curl_multi_wakeup` from another thread. That is a free latency win today: cancellation latency is bounded by `timeouts.shutdown` (2 s default, `curl_transport.cpp:297`), which the architecture never states. |
| 3.4 | "Chains Harness shutdown into it" | **Partly** | This needs one combined token per attempt: a per-attempt `stop_source` plus two `stop_callback`s (the turn token and the jthread token), or two tokens passed side by side. The transport's `AbortCause` distinction (`curl_error.cpp:21-26`) disappears. That is harmless: a cancelled attempt publishes `PublishCancelled` with no message (`turn_machine.cpp:266-270`), and shutdown discards events (`pump.cpp:141-153`). |
| 3.5 | Phase 0 switches cancellation and "every existing test passes unchanged" | **Incorrect** | `Transport::perform` takes `const std::atomic<bool>&` (`core/transport.hpp:43-45`), and every fake overrides it: `tests/support/transport/fake_transport.hpp:71-74`, `tests/integration/runtime_edge_tests.cpp:86-93`, `tests/integration/harness_edge_tests.cpp:104-108`, `testing/src/scripted_transport.cpp:71,122`. Tests build `TurnRoute`s with an atomic and read `cancel_flag()` (`tests/runtime/runtime_tests.cpp:272-298`, `tests/runtime/runtime_test_support.hpp:132`). Either adapt the token back into an atomic inside `HttpBackend`, or move the switch into its own PR. |
| 3.6 | `timeouts.shutdown` bounds destruction | **Confirmed HTTP-only; the guarantee needs rewording** | Consumed only at `curl_transport.cpp:412`→`:297`; validated at `config.cpp:149`. No worker or queue use (`grep` of `src/runtime`, `src/machine`: none). The destruction guarantee (`architecture.md:122-124`, `:512`) becomes "the backend returns promptly once stop is requested", which the runtime cannot bound. An in-process engine could not meet it during prefill (llamad #25 describes the same gap inside the daemon). |

### Configuration split (proposal lines 152-180)

Every field of `Config` (`config.hpp:142-198`), `ResourceLimits` (`:107-128`),
and `TransportTimeouts` (`:83-102`) was traced to where it is consumed:

| Field | Consumed at | Proposed side | Verdict |
|---|---|---|---|
| `base_url`, `api_key`, `extra_headers`, `tls_verify_peer`, `ca_bundle_path`, `proxy` | `provider/*_request.cpp`, `provider/shared.hpp:75-86`, `config.cpp`; `api_key` **also** `worker.cpp:235,332` | http | Confirmed; move the redaction with it |
| `model` | `anthropic_request.cpp:188`, `openai_request.cpp:296`, `config.cpp:50` only | http | **Confirmed** |
| `dialect` | `config.cpp:60,71,128`; `harness.cpp:209`; `scripted_transport.cpp:242` | http | Confirmed |
| `reasoning_mode` | `openai_request.cpp:297`, `config.cpp:66-78` | http | Confirmed as code. As a concept it is portable: llamad toggles `enable_thinking` (`llamad/src/chat_format.cpp:233`). See (f). |
| `sampling` | `harness.cpp:142`, then encoders | runtime | Confirmed. Defaults are HTTP-shaped: a non-optional `temperature{1.0}` always overrides llamad's 0.8 default (`llamad/proto/llamad/v1/llamad.proto:59`) |
| `retry`, `max_tool_rounds`, `tool_round_limit` | `worker.cpp:166-171` | runtime | Confirmed |
| `max_tool_calls_per_turn` | `harness.cpp:134` | runtime | Confirmed |
| `timeouts.*` | `curl_transport.cpp:241-251,297`; validated `config.cpp:149` | http | **Confirmed** (see 3.6) |
| `limits.max_pending_turns` | `harness.cpp:102` | runtime | Confirmed |
| `limits.max_sse_event_bytes`, `max_response_bytes` | `worker.cpp:104`; `curl_transport.cpp:233,400` | http | Confirmed; the worker use moves into `HttpBackend` |
| `limits.max_tool_arguments_bytes` | `worker.cpp:105,169` | runtime | **Partly: needed on both sides** (2.6) |
| `limits.max_tool_result_bytes`, `max_conversation_bytes` | `harness.cpp:106-132` | runtime | Confirmed |
| `limits.max_queued_event_bytes_per_turn` | `worker.cpp:523,531` | runtime | Confirmed |

Also missed: `stop` strings are called llamad-only (proposal line 167), but all
three wire formats support them (Anthropic `stop_sequences`, OpenAI `stop`,
llamad `SamplingParams.stop` at `llamad.proto:65`). They belong in the portable
`SamplingConfig`.

### The HTTP backend and testing (proposal lines 182-196, 292-298)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 4.1 | provider + protocol + transport ≈ 3,100 lines | **Confirmed** | 3,084 lines (`wc -l src/provider/* src/protocol/* src/transport/*`) |
| 4.2 | Phase 0 keeps every test because `create_harness` builds an `HttpBackend` over the scripted transport | **Partly (wrong mechanism)** | `create_harness` is used by only 4 files (`tests/testing/scripted_transport_tests.cpp`, `tests/package_consumer/main.cpp`, `examples/testing_scripted.cpp`, `extras/showcase/npc/main.cpp`). The runtime suites go through `HarnessTestAccess::create(config, adapter, transport, seed, time, tools)`, about 28 calls in 7 files (`tests/support/harness_test_support.hpp:139`, `tests/integration/*`). Some inject custom adapters (`harness_edge_tests.cpp:55,73`). The claim holds if `HarnessTestAccess::create` keeps its signature and wraps the pair in an `HttpBackend`, and the cancellation switch stays out (3.5). `WorkerTimeSource` and the jitter seed stay in the worker (`worker.hpp:25-39`), so retry-timing tests are unaffected. |
| 4.3 | Phase 1 "moves the runtime tests to `ScriptedBackend`" | **Oversimplified** | 19 `requests()` assertions in 5 integration files check encoded bodies (e.g. `tool_loop_tests.cpp`: 8). They test resend encoding and must stay on HTTP. Only the pure runtime behaviour (ordering, budgets, cancellation, commit) moves. Of the 3,303 lines in `tests/integration`, expect roughly half to move. |
| 4.4 | The conformance header can run in llamad CI | **Partly** | llamad tests are plain executables with a `CHECK` macro and no framework (`llamad/AGENTS.md:111-113`, `llamad/tests/CMakeLists.txt:1`). The header must be framework-free, reporting failures through a callback or returning a list, not Catch2. |

### The llamad mapping (proposal lines 212-238)

| Row | Verdict | Evidence |
|---|---|---|
| `system_prompt` as a first `system` message | **Confirmed** | Roles pass through to the template (`llamad/src/chat_format.cpp:62`). The template can inject its own default system prompt when none is sent (`llamad/docs/design.md` "The template is in charge"). A schema turn appends to the first `system` message (`chat_format.cpp:252-259`). |
| user `TextBlock`s as `user` | Confirmed | Join with no separator, as the OpenAI encoder does (`src/provider/openai_request.cpp:84-104`) |
| `ToolResultBlock`s as one `tool` message each | **Partly** | Persisted documents may mix text and results in one user message (`conversation_persistence.cpp:157-206` validates only role versus block type). The OpenAI encoder rejects that mix (`openai_request.cpp:169-172`). The shim must choose the same rejection or emit in block order. |
| `is_error`: nothing to carry | **Confirmed for Scry-made errors** | Scry always sends `{"error": ...}` with `is_error` (`src/runtime/tool_dispatch.cpp:99-116`); llamad's loop sends the same shape (`llamad/client/src/client.cpp:31-39,82-89`). A restored history can carry `is_error = true` with any text, and that flag is lost. |
| assistant as content + `tool_calls` | **Partly** | Both reach `common_chat_msg` (`chat_format.cpp:60-75`). Whether a template renders content alongside calls is template-dependent, and interleaved text/call order (Anthropic histories) is flattened. |
| `ToolDefinition` as `Tool` | Confirmed | `llamad.proto:77-81`. Schema syntax is checked by the daemon (`chat_format.cpp:81-84`). |
| `SamplingConfig` as `SamplingParams`, same names | **Partly** | Types differ: `double`→`float`, `uint32 max_tokens`→`int32` (values above `INT32_MAX` go negative, which llamad reads as "until the context is full", `llamad.proto:64`, `llamad/src/engine.cpp:1053`). Ranges differ: llamad treats temperature ≤ 0 as greedy and `top_p` ≥ 1 as disabled (`llamad.proto:59-61`). `Backend::validate` needs llamad's own checks. |
| text chunk as `text()`; empty chunk silent | **Confirmed**; the daemon sends no empty chunks today | Chat writes only non-empty visible text (`llamad/src/service.cpp:132`, `:160-161`). **Missed:** the final chunk's `text` "may be empty" (`llamad.proto:130-131`), so the shim must forward non-empty final text too. |
| `EOG`/`STOP` as `completed` | **Confirmed** | `STOP` happens without client stop strings: the template's `additional_stops` are appended (`service.cpp:231-232`), and a tool call ended by one is promoted to `TOOL_CALLS` (`:151-157`) |
| `LENGTH` as `length` | **Partly** | When the limit lands inside a call, the calls are dropped (`service.cpp:151-157`, `chat_format.cpp:406-414`). If no text preceded them, the response is empty and Scry fails the turn with `protocol` "model response contained no content" (`turn_machine.cpp:439-442`), not `length`. |
| `TOOL_CALLS` as `tool_use`; arguments canonicalized | **Confirmed** | The iff invariant (`llamad/AGENTS.md:79-82`) matches the machine's check (`turn_machine.cpp:444-449`). IDs are 32 random characters generated per response (`chat_format.cpp:40-53,334`). Formats whose model emits its own IDs could repeat one across rounds, which Scry rejects (`turn_machine.cpp:66-69`). |
| final `CANCELLED`, or stream ends after stop as `cancelled` | **Partly** | The daemon never writes a `CANCELLED` final chunk to a live client: `client_gone` suppresses it (`service.cpp:121-126,165-176`). Returning `cancelled` is correct **only when `stop` was requested** (1.4). |
| stream ends `OK` without a final chunk as `protocol` | Confirmed | Matches `worker.cpp:318-322` |
| `GenerateStats` as `Usage` | Confirmed | `prompt_tokens` includes cached tokens (`llamad.proto:121,127`); Anthropic's `input_tokens` does not. `cached_prompt_tokens` has nowhere to go. |
| gRPC `UNAVAILABLE`, `DEADLINE_EXCEEDED` as retryable `network` | **Partly** | Both come from the client side (`wait_for_ready(false)`, `client.cpp:44-49`), never from the service. Retrying `DEADLINE_EXCEEDED` queues behind the abandoned prefill until #25 lands (issue #25 has a measured 18 s example). Make it non-retryable, or gate the retry on #25. |
| gRPC `RESOURCE_EXHAUSTED` as `resource_limit` | Confirmed | Raised by gRPC's 4 MiB receive limits, not by the service (#20). The client's receive side has the same default. |
| `INVALID_ARGUMENT`, `FAILED_PRECONDITION` as `invalid_config` | **Partly** | `INVALID_ARGUMENT` also covers daemon faults: `llama_decode failed` (`engine.cpp:1036`) and "sampler chain selected no token" (`:564`), all via `guarded` (`service.cpp:48-57`). `FAILED_PRECONDITION` (embedding model or no template, `service.cpp:189,204,207`) is better caught once, at shim construction, via `GetModelInfo` (`llamad.proto:34-43`). |
| gRPC `CANCELLED` as `cancelled` | **Incorrect as written** | Only when stop was requested; otherwise `network` (1.4). The service returns `CANCELLED` only from `Embed` (`service.cpp:264`); for `Chat` it is the client's own `TryCancel`. |
| anything else as non-retryable `network` | **Incorrect** | Not representable (1.3). `INTERNAL` is the one code the service actually produces here (`service.cpp:59,62`), so it needs an explicit row and a category that is not retried. |

### #18 and packaging (proposal lines 200-210)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 5.1 | #18 is the prerequisite: "the shim needs the stubs, not `client.h`" | **Incorrect for the proposed location** | Inside llamad's tree the shim links `llamad_proto` directly (`llamad/CMakeLists.txt:118-131`). #18's actual problem is that the reflection probe and `-freflection` are global (`llamad/CMakeLists.txt:55-71`), which blocks non-reflection consumers. A Scry-based shim requires GCC 16 reflection anyway. A FetchContent consumer already gets `llamad::proto`; the friction is that `add_subdirectory(client)` runs unconditionally (`CMakeLists.txt:133`) and client-only mode requires `nlohmann_json` (`llamad/client/CMakeLists.txt:6-10`). #18 matters only for an out-of-tree shim, a Clang-tooling build of one, or double registration of the proto (the issue's second point). |
| 5.2 | The real prerequisite | **Missed** | A Scry runtime target that configures without libcurl (`CMakeLists.txt:73`). Otherwise `llamad::scry` drags libcurl and Glaze into llamad's CI, which installs only `grpc protobuf` (`llamad/.github/workflows/ci.yml:29`). llamad's dependency rule (`llamad/AGENTS.md:111-114`) also requires the owner's agreement. |
| 5.3 | llamad CI has gRPC/Protobuf and Scry's does not; `tests/consumer` shows FetchContent | Confirmed | `llamad/.github/workflows/ci.yml:29,67`; `llamad/tests/consumer/CMakeLists.txt:10-18`; Scry's toolchain in `docs/contributing.md:8-23` |

### What happens to llamad's client (proposal lines 254-276)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 6.1 | Function reflection "lowers onto `reflection::add<Args>()`, so it is a third registration path, not a new registry" | **Partly: the registry is shared, but the contract changes** | **Descriptions:** Scry's schema is a compile-time constant (`input_schema_v<Args>`, `include/scry/detail/reflection_registration.hpp:81`) that reads descriptions only from `scry::reflection::description` annotations on members (`reflection_schema.hpp:50-57,94-120`). Members synthesized by `define_aggregate` carry no annotations (`llamad/client/include/llamad/client.h:94-98`). llamad passes parameter descriptions positionally at run time (`client.h:100-109,147`). Scry needs a new description-override hook in `reflection_schema.hpp`. **Annotation type:** llamad's `client::desc` (`llamad/client/include/llamad/json.h:33-41`) is not recognized by Scry's template-identity check. **Optionality:** llamad treats `std::optional` as omittable (`json.h:6-8`, `:254-255`). Scry makes `std::optional<T>` required-but-nullable, with only a default member initializer permitting omission (`architecture.md:303-306`); `define_aggregate` cannot supply default initializers, so every synthesized parameter becomes required. **Unknown keys:** ignored by llamad (`json.h:7,301`), rejected by Scry (closed schemas, `architecture.md:308-316`). **Results:** llamad hands a `std::string` result to the model verbatim (`client.h:134-135`); Scry encodes it as a quoted JSON string. **Errors:** llamad forwards exception text to the model (`client.cpp:79-86`); Scry never does (`architecture.md:360-365`). **Callables:** `std::function` (`client.h:194`) versus Scry's move-only `UniqueFunction` (`reflection_registration.hpp:33-52`), which is compatible. The member-function form needs a `ToolCallContext`-leading variant. The mechanics are about 150 lines. The model-visible changes need a decision per point. |
| 6.2 | Keep `Client` for the synchronous paths; retire `ToolSet` | Reasonable | `client.h:303-434`. `ToolSet` is also tested in `llamad/tests/client_tools_test.cpp` and `client_chat_test.cpp`; those move or go with it. |

## Job 2: Alternatives

### (a) Backend-owned concurrency (push events into a sink from any thread)

**What changes.** `perform()` becomes `start(request, sink)` returning a
handle. The backend calls `sink.text()`, `sink.complete()`, or `sink.fail()`
from its own thread: a gRPC completion queue, curl multi driven by the host,
or an engine thread.

**Cost.** The machine still has to be driven by exactly one thread, because it
is single-threaded state (`turn_machine.hpp:153-247`). So sink calls either
lock the machine, or become commands on the command queue that the worker
drains. The second option is today's worker with one more hop. The retry wait
and tool wait (`worker.cpp:338-401`) stay where they are. The following
guarantees get reopened:

- "one worker per Harness owns the network transfers ... FIFO"
  (`architecture.md:47-50`)
- the destruction join (`:102`, `:122-124`)
- the queue terminal reserve (`worker.cpp:491-532`)

The cost is roughly 400-600 lines of worker rework plus a new race surface for
TSan to cover.

**What it buys.** Several turns or Harnesses could share one I/O thread. Scry
deliberately runs one active turn per Harness, and llamad serves one
generation at a time (`llamad/docs/design.md` "v1 serves one generation at a
time"), so neither backend can use that concurrency.

**In-process llama.cpp.** A backend with no gRPC and no daemon is already
possible under the blocking interface: `llamad_engine` and `llamad_chat` are
gRPC-free static libraries (`llamad/CMakeLists.txt:80-93`), and
`Engine::generate` is synchronous with a chunk callback that can cancel. The
worker thread is exactly where that generation should run. The one thing the
blocking interface cannot give such a backend is prompt stop during prefill;
that is #25 again, now inside the host process. An in-process engine also
contradicts "Scry does not load model weights" (`architecture.md:34`) and
llamad's product boundary. Do not design for it; just avoid closing the door:
keep the Scry→llamad message mapping reusable between the gRPC and in-process
variants.

**Verdict: reject.** Where a backend needs its own concurrency (a watchdog
that calls `TryCancel` for an idle bound, since the synchronous `Read()`
blocks), it belongs inside that backend, not in the interface.

### (b) Make the sans-I/O machine plus the pump the product

**What changes.** `TurnMachine`, `MachineEvent`, and `MachineCommand` go public;
each backend ships its own driver loop.

**Cost.** Today's driver is the whole of `worker.cpp` (548 lines). It holds
invariants the machine does not: tool calls published as one batch
(`worker.cpp:457-476`), the terminal-event reserve and trimming (`:34-52`,
`:491-532`), redaction, jitter sampling (`:233-250`), the order of
cancel-command handling, and the snapshot-release rule (2.5). Every backend
would re-implement these, so the guarantees would hold per backend rather than
once. The proposal's stated goal is the opposite: "behave identically over
every backend" (proposal lines 127-129). It also freezes about 250 lines of
internal types (`turn_machine.hpp`) as public API.

**What it buys.** Hosts that want no Scry thread at all (single-threaded or
wasm) could drive the machine themselves. Nobody has asked for that, and it
can be added later as an executor option over the same machine without
exposing it.

**Verdict: reject.** The current shape, where the machine is internal and one
worker drives it, is what makes "one runtime, many backends" true.

### (c) The message model

**Findings.** Scry's two-role block model (`message.hpp:12-59`) is lossless
input for the four-role OpenAI/llamad shape. The OpenAI encoder already does
the flattening (`openai_request.cpp:152-243`), and it is the same function the
llamad shim needs. What cannot be carried in four-role form:

- mixed text and results in one user message (rejected at
  `openai_request.cpp:169-172`)
- `is_error` (only the text carries it)
- the interleaving order of text and calls in an assistant message

Content neither backend supports today: thinking blocks make the Anthropic
decoder fail with `protocol` (`anthropic_content.cpp:73-85`); llamad streams
thinking inline as content for plain and tool turns (`chat_format.cpp:217-225`),
so it is committed and resent as text. There are no images and no cache
markers.

**Cost of enriching now.** Every new `ContentBlock` alternative breaks every
host `std::visit`, bumps the persistence version
(`conversation_persistence.cpp:19`), and forces both backends to encode or
reject it.

**What it buys now.** Nothing: neither backend produces or consumes the new
kinds.

**Verdict: keep the model, with three amendments.**

1. Make the four-role flattening a public helper so every OpenAI-shaped backend
   shares one implementation of the edge cases.
2. Write the rule for new block kinds now: a backend that cannot carry a block
   **rejects** the request with `invalid_config`, and never drops it.
3. Sketch a `ReasoningBlock` (opaque, provider-signed payload plus
   display text) in the proposal. When Anthropic thinking lands it becomes
   additive, and the reject rule means no backend silently drops it.

### (d) The tool layer as its own library

**Findings.** The registry, dispatch, reflection, and JSON code depend only on
`core/` (`src/runtime/tool_registry.cpp:1-3`, `tool_registry_impl.hpp:3`,
`tool_dispatch.hpp:3-4`, `src/reflection/json_bridge.cpp:1`), and export
already needs no Harness (`architecture.md:239-241`).

**Cost.** CMake work only. Split `scry` into `scry::core` (Message, Json,
Error, ToolRegistry, reflection, persistence), `scry::scry` (runtime, no curl),
and `scry::http`. A few hundred lines of CMake plus the package-config
component.

**What it buys.** The curl-free runtime that 5.2 needs. A shim that depends on
the smallest possible surface. A real home for manifest export.

**What it should not do.** llamad's `Client` should not adopt `scry::core`;
that swaps llamad's nlohmann dependency for Scry plus Glaze in its smallest
product.

**Verdict: adopt as a Phase 1 packaging change.** It does not change the
repository-layout recommendation.

### (e) Where the shim lives

| Option | Cost | Buys | Verdict |
|---|---|---|---|
| In llamad as a compiled `llamad::scry` library (proposal) | llamad gains Scry as a dependency, plus Glaze and libcurl unless (d) lands. llamad's owner must agree (`llamad/AGENTS.md:111-114`). A Scry tag pin to bump. | Proto mirrors change in the same commit (`llamad/AGENTS.md:65-69`); the fake-service test pattern already exists (`llamad/tests/CMakeLists.txt:47-52`) | Acceptable |
| In llamad as a **header-only INTERFACE target** whose Scry dependency the consumer supplies | Same test cost; llamad's own libraries link nothing new; only its test fetches Scry | Same as above, plus none of llamad's shipped targets depend on Scry. This matches how `convert.h` already ships beside the proto (`llamad/CMakeLists.txt:116-121`) | **Preferred** |
| In Scry as an optional component | gRPC/Protobuf in an optional Scry leg. `src/**` must stay reflection-free for tooling, so the shim cannot use `convert.h` there. Needs #18 to consume `llamad::proto` cleanly. | Scry's CI proves conformance directly | Reject: the proposal's reasoning holds |
| A third repo `scry-llamad` | One more repo to version and CI; mirror drift across three repos | Both projects' CI fully independent | Only if llamad's owner declines the dependency |
| A header-only shim templated on the stub type, so neither repo depends on the other | Still has to name `grpc::ClientContext` and `grpc::Status`, or template those too; brittle and unusual | Decoupling that the INTERFACE-target option already gives | Reject |

It is also worth a sentence in the proposal: Scry already reaches
`llama-server` through the OpenAI-compatible dialect (`scripts/ci-local-model.sh:3-5`).
The llamad backend is justified by llamad's own contract (a 0600 Unix socket,
statelessness, grammar-enforced tool calls and schemas), not by "local models"
in general.

### (f) Capabilities versus typed request extensions

| Feature | Where it should live | Why |
|---|---|---|
| `response_schema` | Optional per-turn field in `BackendRequest`, supplied through a new per-turn `send()` option | All three wire formats serve it. It is per-turn, not per-Harness. llamad refuses a schema together with tools (`chat_format.cpp:193-198`), and Scry always sends the registry snapshot (`harness.cpp:121,141`), so the runtime must decide what a schema turn means for the tool loop. |
| Capability check | Replace `capabilities()` with `Status Backend::admit(const TurnShape&)`, called in `send()` | A capability flag set cannot express "schema but no tools". A validation hook lets the backend decide and returns a normal admission error (`architecture.md:27-31`). |
| Stop strings | Portable `SamplingConfig::stop` | Supported by all three (see the configuration section) |
| `top_k`, `min_p` | Backend config | Anthropic has `top_k`, OpenAI does not, and `min_p` is llama-only |
| Reasoning controls | Backend config for now | The HTTP semantics (`reasoning_effort: none`) and llamad's (`enable_thinking`, not on the wire) differ; there is no common vocabulary yet |
| Prompt caching | HTTP backend config (e.g. auto-mark the history end with `cache_control`); llamad needs nothing | llamad's KV prefix reuse is automatic (`llamad/AGENTS.md:73-78`) |
| Cached-token usage | A real `Usage::cached_input_tokens` field, not an opaque extension (proposal line 314) | Anthropic, OpenAI, and llamad (`llamad.proto:127`) all report it |

**Verdict:** static knobs stay as typed backend config; add a small typed
per-turn options struct; replace `capabilities()` with an admission hook.

### (g) Other things the code shows

1. **Put the error policy where both sides can see it.** The machine should
   honour a backend's `retryable` as a narrowing: retry only if the category is
   retryable **and** the backend says so. The runtime should also turn an
   unrequested `cancelled` into `network`. Both fixes are about 10 lines in
   `turn_machine.cpp:179-200` and `worker.cpp:233-250`, and they close 1.3 and
   1.4 for every future backend.
2. **Sanitize in the runtime.** Every backend-supplied `message`,
   `provider_detail`, and `request_id` should go through
   `transport_policy::sanitize_provider_detail`-style bounding before it is
   queued. That keeps `error.hpp:42-43` true without trusting each backend.
3. **Keep a convenience constructor.** The public API change breaks every
   example (`examples/main_loop.cpp:307`, `seeded_trials.cpp:35`,
   `tool_policy.cpp:72`, `extras/showcase/npc/main.cpp:84`) and every consumer.
   `scry::http::create_harness(Config, http::Config, ToolRegistry)` keeps the
   common case to one call.
4. **Idle detection for llamad needs a watchdog.** HTTP has an idle bound
   (`curl_transport.cpp:243-244`); llamad has only a whole-call deadline
   (`client.cpp:46-48`). An idle bound over the synchronous `Read()` needs a
   timer thread that calls `TryCancel` (see (a)). Pair it with #19; until then,
   no idle bound, as the proposal says.
5. **`Error::http_status` stays.** Document it as zero for non-HTTP backends
   rather than generalizing it now.
6. **The worker gets simpler.** Replacing adapter plus transport with one
   backend drops `WorkerActor`'s constructor from six arguments to five
   (`worker.hpp:43-46`), which helps the lizard gate.

## Recommended amendments to backends.md

1. **Error contract.** Replace "What each backend supplies through `Error`"
   (lines 135-138) with:
   - the machine retries only when the category is retryable **and** the
     backend's `retryable` is true (implement in `turn_machine.cpp` `correlate`);
   - `cancelled` is honoured only when stop was requested, and is otherwise
     rewritten to `network`;
   - the runtime sanitizes and bounds every backend string.

   Update `architecture.md` "Turn processing and retries" in the same change.
2. **Stream contract.** Add to the `BackendStream` docs:
   - `text("")` is ignored and does not imply semantic output;
   - `semantic_output()` may come before or after text, but must come before
     `perform()` returns an error once any content was received (`HttpBackend`
     calls it on every `semantic_output_consumed` flip, including Anthropic's
     empty-block start);
   - the concatenated deltas must equal the returned text content.

   The conformance suite asserts all three.
3. **`BackendRequest`.** Add `max_tool_arguments_bytes` (or a `limits` view).
   State that the worker holds the request snapshot for the duration of
   `perform()` and releases it before applying `ModelCompleted`.
4. **"What the code already says".** List the missed runtime dependencies from
   1.7 and assign each one:
   - redaction and the request-id fallback go to `HttpBackend`;
   - HTTP validation goes to `scry::http::validate`, and runtime validation
     keeps only retry, limits, and tool bounds;
   - curl status and jitter seeding: `make_backend` returns the curl error, and
     the seed identity uses the `Impl` pointer;
   - `find_package(CURL)` becomes conditional on the HTTP component.
5. **Cancellation.**
   - The stop source is created in `send()` on the host, not owned by the
     worker.
   - The worker builds a per-attempt combined token.
   - Document the stop-callback obligations: `noexcept`, non-blocking,
     thread-safe, run on the host inside `cancel()`, `update()`, and
     `~Harness`, and must not take locks the worker holds.
   - `HttpBackend` registers `curl_multi_wakeup` so cancel latency is no longer
     bounded by `timeouts.shutdown`.
   - Rewrite `architecture.md:62-63`, `:122-124`, and `:512` as backend
     obligations.
6. **Phase 0.** Split it:
   - **0a** extracts `detail::Backend` and `HttpBackend` and keeps
     `HarnessTestAccess::create`'s signature by wrapping adapter plus transport
     (this is what keeps the tests unchanged, not `create_harness`);
   - **0b** switches to the stop token and updates the five transport fakes and
     the `cancel_flag` tests.
7. **Phase 1 tests.** Tests asserting encoded requests stay on HTTP; only
   runtime-behaviour tests move to `ScriptedBackend`. The conformance header is
   framework-free so llamad's `CHECK`-based tests can run it.
8. **Configuration split.**
   - Mark `max_tool_arguments_bytes` as needed on both sides.
   - Confirm `timeouts.shutdown` as HTTP-only.
   - Move `stop` into the portable `SamplingConfig`.
   - Make `temperature` optional so each backend's default applies.
   - Keep `scry::http::create_harness(...)` as the one-call path.
   - Rewrite `Harness::validate`'s documentation.
9. **Packaging.** Split into `scry::core` / `scry::scry` (curl-free) /
   `scry::http`. That, not llamad #18, is the Phase 2 prerequisite. Reword the
   #18 paragraph: it matters only for out-of-tree or Clang-tooling consumers.
10. **Shim location.** Keep it in llamad, but as a header-only INTERFACE target
    whose Scry dependency the consumer supplies, with only llamad's tests
    fetching Scry. Record that this needs llamad's owner to approve the
    dependency under `llamad/AGENTS.md:111-114`.
11. **llamad mapping table.** Correct or add these rows:
    - gRPC `CANCELLED` → `cancelled` only when stop was requested, otherwise
      `network`;
    - `INTERNAL` → a non-retryable category;
    - `DEADLINE_EXCEEDED` → not retried until #25 lands;
    - `FAILED_PRECONDITION` → detected at construction via `GetModelInfo`;
    - forward non-empty final-chunk text;
    - an unknown future finish reason → `unknown`;
    - `LENGTH` inside a call with no text → the `protocol` failure it will
      produce;
    - text blocks joined with no separator;
    - mixed text and results in one user message → rejected, as in the OpenAI
      encoder;
    - `max_tokens` and float conversion with range checks in
      `Backend::validate`;
    - thinking text arrives inline as content.
12. **Structured output.** Make `response_schema` a per-turn `send()` option,
    replace `capabilities()` with `Backend::admit(const TurnShape&)`, and
    decide what a schema turn means for tools, since llamad refuses both
    together.
13. **Usage.** Add `cached_input_tokens` as a real field instead of an opaque
    extension.
14. **Message model.** Keep it. Publish the four-role flattening as a shared
    helper. Adopt "reject, never drop" for block kinds a backend cannot carry,
    and sketch a future `ReasoningBlock`.
15. **Phase 3 reflection.** Retitle it as a port. List the decisions it needs:
    - a description-override hook in `reflection_schema.hpp`;
    - whether `std::optional` parameters are omittable;
    - how `std::string` results are encoded (raw text or JSON string);
    - whether exception text reaches the model;
    - unknown-key strictness.

    Each one changes what llamad's current users' models see.
16. **Alternatives considered.** Add a short section recording that
    backend-owned concurrency, a public machine, and a richer message model
    were considered and rejected, with the reasons from (a)-(c), so the
    question is not reopened in each phase.
