# Proposal: Scry over pluggable backends

Status: proposal, not a description of the current tree. `architecture.md`
remains the source of truth until a phase below lands.

## The idea, restated

Scry today is two things fused together:

1. **The runtime.** A turn machine, retries, transactional history, a tool
   registry with reflected and explicit handlers, admission and byte limits, and
   a pump that delivers everything on the host thread. None of this cares how a
   model request reaches a model.
2. **An HTTP client for two chat APIs.** libcurl, an SSE parser, and request
   encoders and stream decoders for the Anthropic Messages API and the
   OpenAI-compatible Chat Completions API.

llamad is a third thing: a local inference server behind gRPC on a Unix socket,
plus a client library that reimplements a smaller version of (1) on top of it
(`ToolSet`, the execute-and-resend loop, function-signature reflection, typed
replies).

The proposal is to make (1) the whole of Scry, make (2) one backend, and make a
llamad shim the second backend, so that a host writes tools and drives turns the
same way whichever backend it links.

One correction to the framing: the backend interface should belong to Scry, not
to each backend. If every backend defined its own interface, Scry would need an
adapter per backend anyway and the adapters would be the real interface. One
interface, defined by the consumer, implemented by each backend, is what makes a
new backend a few hundred lines.

## What the code already says

The seam the proposal needs already exists inside Scry; it is private and sits
one level too low.

`src/runtime/worker.cpp` `perform_attempt()` composes three HTTP-shaped pieces
for every model request:

```
ProviderAdapter::make_request(Config, ModelRequest) -> TransportRequest {url, headers, body}
Transport::perform(TransportRequest, stop_token, atomic<bool> cancelled, BodyChunkSink)
SseParser::push(bytes) -> SseEvent
ProviderAdapter::parse_stream_event(name, data, DecodeState, out) -> ProviderTextDelta | ProviderCompleted{ModelResponse}
```

Everything above that point is backend-neutral. The turn machine
(`src/machine/turn_machine.hpp`) consumes exactly four things from a model
attempt: `ModelTextDelta`, `ModelSemanticOutput` (the point after which a
failure is no longer retried), `ModelCompleted{ModelResponse}`, and
`AttemptFailed{Error, retry_after}`. `ModelRequest` (`src/core/model.hpp`) is
system prompt, history snapshot, this turn's messages, tool schema snapshot, and
sampling. `ModelResponse` is content blocks, finish reason, usage, request id.

That pair, `ModelRequest -> stream of text deltas -> ModelResponse | Error`, is
the backend interface. It is not `Transport` (HTTP-shaped: URL, headers, body,
bytes) and not `ProviderAdapter` (SSE-shaped). A llamad backend would have to
fake an HTTP response and SSE frames to fit either of those. It fits the level
above them directly, because llamad's stream contract (zero or more text chunks,
one final chunk with finish reason, stats, and whole tool calls) is already
that shape.

On the llamad side, `AGENTS.md` boundary 4 (stateless daemon, clients resend
history, tools are opaque per-request data) is precisely what Scry's immutable
history snapshots and per-turn schema snapshots produce. Scry never asks a
backend to remember anything. Issue #18 in llamad already describes the
consumer this proposal creates: "a runtime that speaks the daemon's contract
without going through `client.h`, one that already has its own tool loop, retry
policy, or threading model and only needs the `Chat` stream."

## The interface

Public header `include/scry/backend.hpp`. Names are proposals.

```cpp
namespace scry {

/// One model request as the runtime hands it to a backend. Borrowed for the
/// duration of perform(); the runtime owns every snapshot it points at.
struct BackendRequest {
  std::string_view system_prompt;
  std::span<const Message> history;        // committed, immutable
  std::span<const Message> messages;       // this turn's exchange so far
  std::span<const ToolDefinition> tools;
  const SamplingConfig& sampling;
};

/// What a backend produces when an attempt ends normally.
struct BackendResponse {
  std::vector<ContentBlock> content;       // text and tool-call blocks
  FinishReason finish_reason;              // completed, length, tool_use, unknown
  Usage usage;
  std::string request_id;                  // sanitized, may be empty
};

/// The runtime's side of a streaming attempt.
class BackendStream {
public:
  /// A fragment of assistant text. A failure means the runtime cannot accept
  /// more (a queued-event limit); the backend abandons the attempt and returns it.
  virtual Status text(std::string_view delta) = 0;
  /// The response has started carrying semantic content (text or a tool call).
  /// After this, a failed attempt is not retried. text() implies it.
  virtual void semantic_output() = 0;
};

class Backend {
public:
  virtual ~Backend() = default;
  /// The checks Harness::create() and Harness::validate() run on what the
  /// backend will be asked to send. Today's dialect-conditional checks on
  /// SamplingConfig (seed, max_tokens, reasoning) move here.
  [[nodiscard]] virtual Status validate(const SamplingConfig&) const = 0;
  /// One attempt. Blocks the worker until the response is complete or the
  /// attempt fails. `stop` is requested on turn cancellation and on Harness
  /// shutdown; the backend must return promptly once it is.
  [[nodiscard]] virtual Result<BackendResponse>
  perform(const BackendRequest&, BackendStream&, std::stop_token stop) = 0;
};

} // namespace scry
```

What the interface deliberately leaves out:

- Tool execution, admission hooks, the round limit, byte limits on history and
  tool payloads, retries and backoff, cancellation semantics, history commit.
  All of these stay in the runtime and behave identically over every backend.
- Any notion of URL, socket, credential, dialect, or model name. Those are
  backend configuration.
- Embeddings, tokenization, model info. They are not turns. A host that needs
  them uses the backend's own client (llamad's `Client::embed`) beside Scry.

What each backend supplies through `Error`: `category`, `retryable`,
`retry_after`, `provider_detail` (namespaced, sanitized), and `request_id`. The
HTTP backend already does this in `transport_policy`; the llamad backend maps
gRPC status codes.

Two runtime changes come with the interface:

- **Cancellation becomes a `std::stop_token`.** Today the worker polls a
  `shared_ptr<std::atomic<bool>>` inside curl callbacks. gRPC's synchronous
  reader blocks in `Read()`, and llamad's own client already cancels it with a
  `std::stop_callback` that calls `ClientContext::TryCancel()`. A token serves
  both. The worker owns one `std::stop_source` per turn, requests it on
  `Turn::cancel()`, and chains Harness shutdown into it.
- **`ModelRequest` and `ModelResponse` go public** as `BackendRequest` and
  `BackendResponse`, or the detail types are renamed. `ContentBlock`,
  `Message`, `ToolDefinition`, `FinishReason`, and `Usage` are public already.

## Configuration split

`Config` today mixes runtime settings with HTTP client settings. Split it.

| Stays in `scry::Config` (runtime) | Moves to `scry::http::Config` |
|---|---|
| `sampling` | `base_url`, `api_key`, `model` |
| `retry` | `dialect`, `reasoning_mode` |
| `max_tool_rounds`, `max_tool_calls_per_turn`, `tool_round_limit` | `timeouts` (connect, idle, transfer, shutdown poll) |
| `limits.max_pending_turns` | `tls_verify_peer`, `ca_bundle_path`, `proxy`, `extra_headers` |
| `limits.max_tool_arguments_bytes`, `max_tool_result_bytes` | `limits.max_sse_event_bytes`, `max_response_bytes` |
| `limits.max_queued_event_bytes_per_turn`, `max_conversation_bytes` | |

`model` belongs to the HTTP backend: llamad serves one model and has no name for
it. `reasoning_mode` is an OpenAI request field today. A backend that has
sampling knobs Scry's `SamplingConfig` lacks (llamad: `top_k`, `min_p`, stop
strings) puts them in its own config; Scry's `SamplingConfig` is the portable
subset and stays small.

Construction becomes:

```cpp
auto backend = scry::http::make_backend({.base_url = ..., .api_key = ..., .model = ..., .dialect = ...});
auto harness = scry::Harness::create(config, std::move(*backend), std::move(tools));
```

`Harness::validate(config)` keeps validating the runtime config;
`scry::http::validate(http::Config)` validates the HTTP one. `Harness::create`
also calls `backend->validate(config.sampling)`.

## The HTTP backend

`src/provider/`, `src/protocol/`, and `src/transport/` (about 3,100 lines)
become `scry::http`, a second static library in the same repository, linked by
default. `HttpBackend::perform()` is today's `perform_attempt()` minus the
machine calls: encode, transfer, parse SSE, decode, forward deltas to the
stream, return the completed response. The worker shrinks to the machine loop.

`scry::testing::ScriptedTransport` keeps its role: it substitutes the HTTP
transfer under the HTTP backend, and the HTTP backend's own tests (dialect
encoding, SSE splitting, status classification) keep using it. The runtime's
tests move to a new `scry::testing::ScriptedBackend`, a queue of scripted
`BackendResponse`s, text deltas, holds, and failures at the new seam. Those
tests then prove the runtime's guarantees for every backend at once, which is
the point of the split.

## The llamad backend

A shim of a few hundred lines that links `llamad::proto` (the generated gRPC
stubs) and implements `scry::Backend`. It lives in the llamad repository as
`llamad::scry` (a target beside `llamad::client`), for three reasons: it is a
mirror of `llamad.proto` and boundary 3 in llamad's `AGENTS.md` already says
mirrors change with the proto in the same commit; llamad's CI already has gRPC
and Protobuf and Scry's does not; and llamad's `tests/consumer/` already shows
how to build against the repository with `FetchContent`. Scry becomes a pinned
dependency of that target only.

Prerequisite in llamad: issue #18, a proto-only target that builds without the
reflected client. The shim needs the stubs, not `client.h`.

The mapping, which the shim owns entirely:

| Scry | llamad |
|---|---|
| `system_prompt` | `ChatMessage{role: "system"}` first, when non-empty |
| user `Message` with `TextBlock`s | `ChatMessage{role: "user", content}` |
| user `Message` with `ToolResultBlock`s | one `ChatMessage{role: "tool", content: result.text, tool_call_id}` per block |
| `ToolResultBlock::is_error` | nothing to carry; Scry already shapes error results as `{"error": ...}` text, which is what llamad's own loop sends |
| assistant `Message` | `ChatMessage{role: "assistant", content: text blocks joined, tool_calls: [{id, name, arguments.text}]}` |
| `ToolDefinition{name, description, input_schema}` | `Tool{name, description, parameters_json_schema}` |
| `SamplingConfig{temperature, top_p, max_tokens, seed}` | `SamplingParams`, same names; `top_k`, `min_p`, `stop` from the backend's own config |
| text chunk | `BackendStream::text()`; an empty chunk (issue #19 progress) is consumed silently |
| final chunk `EOG` or `STOP` | `finish_reason = completed` |
| final chunk `LENGTH` | `finish_reason = length` |
| final chunk `TOOL_CALLS` | `finish_reason = tool_use`, one `ToolCallBlock` per call; the machine canonicalizes the arguments and rejects a non-object as it does today |
| final chunk `CANCELLED`, or stream ends after `stop` was requested | `Error{cancelled}` |
| stream ends `OK` without a final chunk | `Error{protocol}` |
| `GenerateStats{prompt_tokens, completion_tokens}` | `Usage{input_tokens, output_tokens}` |
| gRPC `UNAVAILABLE`, `DEADLINE_EXCEEDED` | `Error{network, retryable}` |
| gRPC `RESOURCE_EXHAUSTED` (issue #20) | `Error{resource_limit}` |
| gRPC `INVALID_ARGUMENT`, `FAILED_PRECONDITION` | `Error{invalid_config, provider_detail: "llamad:<code>"}` |
| gRPC `CANCELLED` | `Error{cancelled}` |
| anything else | `Error{network}`, not retryable |

Backend config: socket path, optional per-attempt deadline (default none,
because a whole-call deadline cannot distinguish a long prefill from a wedged
daemon until issue #19 lands), and the llamad-only sampling fields.

Two llamad issues improve the experience under Scry without blocking it: #25
(a cancelled request keeps decoding and blocks the queue) makes `Turn::cancel()`
cheap for the daemon, and #19 gives Scry a heartbeat during prefill. #21
(distinguish a prompt that does not fit the context) would let the shim report
`resource_limit` instead of `invalid_config` for a history that outgrew the
model.

The shim is verified two ways, both without a model: a conformance suite Scry
ships as a header (`scry/testing/backend_conformance.hpp`, parameterized by a
backend factory, asserting the stream and error contract above), run in llamad's
CI against a scripted `llamad::v1::Llama::Service` on a private socket exactly
as `tests/client_chat_test.cpp` does today; and the runtime's own
`ScriptedBackend` tests, which need no llamad at all.

## What happens to llamad's client

After the shim exists, llamad's `client.h` carries a second, smaller agentic
runtime: `ToolSet`, the `chat(history, tools, ...)` loop, and the
function-signature reflection in `detail::run`. That is the duplication the
split is meant to remove, and it is a product decision rather than a code one.
The recommendation:

1. Keep `Client` as the tiny synchronous path: `generate`, `chat` without
   tools, `chat<T>`, `embed`, `tokenize`, `get_model_info`, `CallOptions`.
   These are what a small program wants and what Scry does not cover.
2. Retire `ToolSet` and the tool-loop overload once the shim ships and
   `llamad-chat --demo-tools` has a Scry-based replacement. llamad's README
   then points tool users at Scry.
3. Port the one thing llamad's tool layer does better into Scry: registering a
   function by reflection (`tools.add<^^get_current_time>()`), with the name
   from the identifier, descriptions from parameter annotations, and the
   argument struct synthesized from the parameter list. It lowers onto Scry's
   existing `reflection::add<Args>()`, so it is a third registration path, not a
   new registry.

`json.h` (nlohmann) stays in llamad for `chat<T>` until Scry has structured
output (below), then can go with it.

## Structured output

llamad's typed reply (`response_json_schema`, enforced by a grammar) is a real
feature Scry lacks, and both HTTP dialects can serve it (OpenAI-compatible
`response_format: json_schema`; Anthropic structured outputs). It fits the
interface as an optional `BackendRequest::response_schema` and a
`Backend::capabilities()` query so `send()` can reject it on a backend that
cannot enforce it. It is listed here so the interface is designed with the
field in mind, and it is the last phase, not the first.

## Phases

Each phase leaves both repositories buildable and releasable on its own.

**Phase 0, Scry, internal, no behavior change.** Introduce `detail::Backend`,
move `perform_attempt()`'s composition of encoder, transport, SSE, and decoder
into `HttpBackend`, hand the worker a `unique_ptr<Backend>`. Switch turn
cancellation to a `std::stop_source`. Every existing test passes unchanged,
because `testing::create_harness` builds an `HttpBackend` over the scripted
transport. This is a refactor PR and the riskiest one for regressions, so it
goes first and alone.

**Phase 1, Scry, public API, 0.6.0.** `include/scry/backend.hpp`; the `Config`
split; `Harness::create(Config, unique_ptr<Backend>, ToolRegistry)`;
`scry::http` as a component with `make_backend` and `validate`;
`testing::ScriptedBackend`; the conformance header. Move the runtime tests to
`ScriptedBackend`. Update `architecture.md` (Public API, Providers and
transport, Build and package), every example, and the release note. The
showcase follows.

**Phase 2, llamad.** Land #18. Add `llamad::scry` with the mapping above, the
conformance run in CI, and an example that drives `llamad` through Scry with a
tool. Pin Scry by tag.

**Phase 3, convergence.** Retire llamad's `ToolSet` and loop; add
function-reflection registration to Scry; add `response_schema` and
capabilities; consider whether `Usage` should carry backend-specific metrics
(llamad's cached prompt tokens and timings) as an opaque extension rather than
growing the struct.

## Decisions to make before Phase 1

- **Repository layout.** This proposal keeps the HTTP backend in Scry and puts
  the llamad shim in llamad. The alternative, a `scry-llamad` repository, keeps
  each project's CI independent at the cost of a third repository to version.
  Putting the shim inside Scry is not recommended: it drags gRPC and Protobuf
  into Scry's build and clang-tooling legs.
- **Names.** `scry::Backend` for the interface, `scry::http` for the HTTP
  backend and its config, `llamad::scry` for the shim. "scry-backend" as a name
  for the HTTP piece disappears: it is the HTTP backend, one of two.
- **The fate of llamad's tool layer** (previous section).
- **Whether `Turn::cancel()` semantics change at all.** They should not. The
  token is an implementation of the same cooperative contract; the guarantees
  in `architecture.md` under Threading and lifetime stay word for word.
