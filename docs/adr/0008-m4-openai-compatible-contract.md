# ADR 0008: M4 OpenAI-Compatible Chat Completions Contract

- Status: Accepted
- Date: 2026-07-18
- Amended: 2026-07-19 — the production provider seam is streaming-only

## Context

M1 established a provider-neutral model and an internal adapter seam, but only
the Anthropic Messages dialect is live. M4 must prove that applications can
switch to OpenAI or a local OpenAI-compatible server by changing `Config`
alone. The intended local targets are vLLM, Ollama, llama.cpp server, and LM
Studio.

"OpenAI-compatible" is not a promise of complete API parity. Those servers
implement overlapping subsets, and tool behavior also depends on the selected
model and chat template. Scry therefore needs one explicit common contract,
strict decoding at its boundary, and deterministic fixtures rather than
provider-specific guessing.

## Decision

### Endpoint, authentication, and sampling

For `ProviderDialect::openai_compatible`, Scry removes trailing slashes from
`Config::base_url` and normalizes exactly these forms:

- an endpoint ending in `/v1/chat/completions` is used unchanged;
- a base ending in `/v1` gains `/chat/completions`; and
- every other valid HTTP(S) origin or path gains `/v1/chat/completions`.

Scry does not infer Azure deployments or accept a bare `/chat/completions`
suffix as a complete endpoint. The existing URL validation continues to reject
whitespace, query strings, fragments, missing authorities, and non-HTTP(S)
schemes.

`api_key` is optional for this dialect because local servers commonly run
without authentication. A nonempty key must contain no CR or LF and produces
`Authorization: Bearer <key>`; an empty key produces no authorization header.
Requests always send JSON content type and the event-stream accept header. The
provider seam has no request-mode toggle.

OpenAI-compatible sampling accepts finite `temperature` in `[0, 2]`, optional
finite `top_p` in `[0, 1]`, and a present positive `max_tokens`. Anthropic keeps
its existing, narrower validation. M4 sends the legacy `max_tokens` field
because it is the broadest local-server denominator; newer reasoning-model
fields are outside this compatibility contract.

### Request mapping

The adapter sends only the common subset:

- model, messages, temperature, optional top-p, positive max-tokens, and
  `stream: true`;
- `stream_options: {"include_usage": true}`; and
- optional function tools with name, description, and parameters.

Scry does not send `n`, `strict`, `parallel_tool_calls`, `tool_choice`, response
formats, log probabilities, or provider-specific extensions. Optional values
are omitted rather than encoded as `null`.

The system prompt becomes the first `role: "system"` message. Neutral user and
assistant text blocks are concatenated in block order without inserted
separators. Assistant tool calls become OpenAI function tool calls with their
stable IDs. Every neutral tool-result block expands into a separate
`role: "tool"` message containing its call ID and canonical JSON result as a
string. Multiple results preserve provider order. A user message that mixes
text and tool results is rejected because translating it would require
reordering semantic content.

OpenAI Chat Completions has no portable `is_error` field. Scry does not add a
nonstandard extension; Scry-generated errors remain visible because their
canonical result is already an error JSON object.

### Streaming-only response scope

The production adapter accepts only Chat Completions SSE responses and does not
expose or maintain a parser for JSON `chat.completion` responses. Reintroducing
a non-streaming decoder requires the evolution-register trigger: a supported
deployment that cannot serve SSE or a concrete consumer requirement, together
with a runtime mode that exercises the decoder and its golden and fuzz
coverage.

Finish reasons map as follows:

| Wire value | Scry value |
|---|---|
| `stop` | `completed` |
| `length` | `length` |
| `tool_calls` | `tool_use` |
| `content_filter` or an unknown future string | `unknown` |
| deprecated `function_call` | protocol error |

`prompt_tokens` and `completion_tokens` from a streaming usage chunk map to Scry
input and output usage. Each usage object is a total and replaces prior totals;
missing usage remains zero. Detailed token breakdowns and `total_tokens` are
not required. A chunk's `chatcmpl-*` ID is not an HTTP request identifier and
does not populate `provider_request_id`; the transport's sanitized response
header remains authoritative.

### Streaming lifecycle

Normal Chat Completions chunks arrive as unnamed or `message` SSE events.
Unknown named optional events produce the existing internal ignored-event
marker. A named error event or a root error object becomes a sanitized Scry
error.

`[DONE]` is the sole successful terminal marker. A non-null finish reason must
arrive before it. The adapter does not complete on the finish chunk because an
empty-choice usage chunk may follow. Duplicate `[DONE]`, content after
completion, `[DONE]` before a finish reason, and stream EOF without `[DONE]`
are protocol failures.

Each data chunk must be a `chat.completion.chunk` with a stable nonempty chunk
ID. It contains either one choice at index zero or an empty choices array with
usage. Role-only and empty initial deltas are accepted; an explicit role must
be assistant. Text is appended to one neutral text block and emitted as text
deltas. After a finish reason, only usage-only chunks and `[DONE]` are valid.

Tool-call accumulators are keyed by the wire index. Fragments may be
interleaved or arrive out of index order. Repeated identical ID, name, and type
metadata is accepted; conflicts are rejected. Argument bytes are checked
against the configured bound before append. At the finish chunk, indices must
be contiguous from zero, IDs and names must be nonempty, type must be
`function`, and each argument string must canonicalize as a JSON object. Empty
arguments normalize to `{}` for zero-argument local-model calls. Completed
calls enter the neutral response in ascending index order.

### Errors, state, and isolation

HTTP status classification remains transport-owned. In-band error `type` or
`code` fields may contribute only a bounded sanitized provider-detail token;
raw provider messages and bodies never cross the adapter. Recognized
authentication, rate-limit, and overload/server errors map to existing Scry
categories; unknown error objects are protocol failures. The adapter checks
both safe string fields for a recognized alias, with `type` winning only when
both fields carry recognized but conflicting categories. Otherwise a
recognized `code` overrides an unknown `type`; the selected alias also becomes
the provider-detail token.

Adapters remain stateless. `ProviderDecodeState` owns common bounds and output
state plus a dialect-specific state variant. Each adapter initializes and
validates only its own alternative. This prevents an Anthropic lifecycle flag
from becoming an accidental OpenAI contract and preserves independent
different-dialect Harness instances.

## Verification

M4 verification includes:

- exact request goldens for endpoint, headers, auth, sampling, system/text,
  tools, tool calls, and expanded tool-result messages;
- streaming fixtures for role/text/usage, finish-reason mapping, `[DONE]`,
  fragmented and interleaved tool calls, exact byte limits, metadata conflicts,
  sparse indices, malformed arguments, error objects, rejected legacy fields,
  malformed chunk shapes, and illegal lifecycle transitions;
- arbitrary-split and short fuzz coverage at the OpenAI wire boundary;
- a public config-only dialect switch, concurrent cross-dialect Harnesses, a
  full OpenAI-compatible tool round, and a Curl loopback path/header/SSE test;
  and
- a scheduled, bounded local-model smoke test. Per-commit tests remain
  deterministic and network-free.

## Consequences

The adapter supports a documented compatibility subset, not every OpenAI or
local-server extension. Adding Responses API, Azure endpoint shapes, reasoning
model token fields, structured outputs, or provider-specific tool controls
requires a new contract rather than silent wire drift.

Supported deployments must expose Chat Completions SSE. A target that genuinely
cannot serve SSE triggers the explicit evolution path above; it does not add an
untested parallel decoder to the existing adapter seam.

The internal provider factory remains config-keyed. A public adapter plugin
surface is still deferred until a concrete third-party provider cannot be
upstreamed.
