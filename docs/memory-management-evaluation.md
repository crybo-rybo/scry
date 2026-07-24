# Memory-Management Performance Evaluation

**Scope:** `src/` and the public headers in `include/scry/`, evaluated for
memory-management performance and efficiency: allocation churn, redundant
copies, retention footprint, and data-structure choices on the hot paths.
No code was changed; every finding cites the current source. Line numbers
refer to the tree at commit `16f92f7`.

**Method:** manual read of the full runtime (`src/runtime`, `src/machine`),
provider seam (`src/provider`, `src/protocol`), transport (`src/transport`),
codecs (`src/core`), and the public headers including the reflection codec.
No profiler was run in this pass, so the "hypothetical gain" figures are
derived from operation counts and payload sizes, not measurements; §9
proposes how to verify them.

**Contract constraints respected by every proposal:** transactional
Conversation commits (API-011), immutable accepted-turn snapshots (API-012),
borrowed callback arguments (API-013), per-turn byte budgets (NET-008), and
the no-throw-across-thread-boundary rule. None of the resolutions below
require weakening these; most are ownership-transfer fixes that preserve
value semantics exactly.

---

## 1. Executive summary

The codebase is disciplined about *bounding* memory (per-turn byte budgets,
delta coalescing, terminal-event reserve) but generous about *copying* it.
The dominant inefficiencies are:

1. **The conversation history is deep-copied ~4× per model attempt and the
   request is re-encoded through a generic JSON DOM that is itself copied
   once more by lvalue assignments** (§2, §3). For a long conversation this
   is the largest allocation source in the library, and it repeats on every
   retry and every tool round.
2. **The streaming text-delta path performs a DOM parse plus ~6–8 heap
   allocations per delta**, of which only ~2 are inherent (§4). Deltas are
   the highest-frequency event in the system.
3. **The completed turn's content is deep-copied four times between the
   provider stream and the committed Conversation** (§2.2–§2.3), where one
   move-based handoff would do.
4. **Every attempt builds fresh libcurl easy+multi handles**, discarding
   the connection cache and TLS session (§6) — a wall-clock cost with an
   allocation-churn component.
5. **`UniqueFunction` has no small-buffer optimization**, so every callback
   registration, tool handler, and per-attempt body sink heap-allocates
   (§5).

A worked estimate for a representative turn (100 KB history, two tool
rounds, one retry, 1,500 streamed deltas) puts the current cost at roughly
**1.5–2 MB of redundant memcpy and 20–40k heap allocations**, reducible to
roughly **0.4 MB and 3–5k allocations** with the resolutions in this report
(§8). The fixes are ordered so that the first tier is purely mechanical
(add `std::move`, add rvalue overloads) and does not disturb any public
contract.

---

## 2. Turn-lifecycle deep copies (highest impact, scales with history size)

### 2.1 The full `ModelRequest` is copied on every attempt

`TurnMachine::issue_attempt()` embeds a copy of the entire request — model
string, system prompt, **all messages**, and **all tool schemas** — into the
`IssueModelRequest` command on every attempt:

```
src/machine/turn_machine.cpp:298-307
  return applied(IssueModelRequest{
      .turn_id = turn_id_,
      .request = request_,        // deep copy of the whole conversation
      .attempt = attempt_count_,
  });
```

This fires for the first attempt, for **every retry**, and for **every tool
round** (`start_request()` → `issue_attempt()` after each round). The sole
consumer only reads it:

```
src/runtime/worker.cpp:267
  auto request = provider_->make_request(config_, issue.request);
```

The machine outlives the attempt and both objects live on the same worker
thread, so the copy buys nothing.

**Resolution.** Drop the payload from the command. Either let the worker
pull `const ModelRequest&` from the machine (`machine.request()`), or carry
`std::shared_ptr<const ModelRequest>` that the machine refreshes only when
`request_.messages` actually grows (once per tool round). As a side effect,
the `MachineCommand` variant shrinks from ~160 bytes (sized by
`IssueModelRequest`) to roughly the size of `CommitCompletion`'s header,
making every `std::deque<MachineCommand>` node cheaper.

**Hypothetical gain.** Eliminates `O(history)` bytes of memcpy plus one
string/vector allocation *per message, block, and schema* on every attempt.
For a 100 KB history with 3 extra attempts (2 tool rounds + 1 retry): about
400 KB of copying and several hundred allocations per turn removed.

### 2.2 The completed exchange is copied twice after the machine already moved it

The machine correctly *moves* `exchange_` into `CommitCompletion`
(`src/machine/turn_machine.cpp:360-367`). The worker then copies it because
`publish_command` takes the command by `const&` even though the caller owns
it and discards it immediately afterwards:

```
src/runtime/worker_publish.cpp:167-177
  if (const auto* completion = std::get_if<CommitCompletion>(&command)) {
    if (!events_->push(
            CompletionEvent{
                ...
                .exchange = completion->exchange,   // deep copy #1
```

(`process_machine_command` at `src/runtime/worker.cpp:203-205` pops the
command off the deque by value — it is an expiring object.)

The pump then copies every message again while committing:

```
src/runtime/pump_state.cpp:222-225
  for (const auto& message : event.exchange) {
    conversation.messages.push_back(message);       // deep copy #2
```

The pending `CompletionEvent` retained for callback delivery only needs the
final text, finish reason, usage, and IDs (`completion_text` at
`src/runtime/pump.cpp:35-47` reads just `exchange.back()`).

**Resolution.** Add an rvalue path: `publish_command(MachineCommand&&)`
moves the exchange into the `CompletionEvent`; in
`PumpState::apply_terminal` (which already receives the event as mutable
`WorkerEvent&`), move the messages into the Conversation and compute the
completion text at commit time, leaving the pending callback event holding
only the small completion fields. Byte accounting is unaffected —
`accounted_bytes` is captured before the move.

**Hypothetical gain.** Two full deep copies of every committed turn's
content removed (for a 20 KB exchange: 40 KB of copying plus one
allocation per string/block per message, twice). Also removes the transient
3×-retention spike at commit (event copy + conversation copy + machine's
copy) discussed in §2.5.

### 2.3 The accumulated `ModelResponse` is copied twice at stream end

Both stream decoders copy the fully-accumulated response into the terminal
provider event, and the worker copies it once more:

```
src/provider/anthropic_stream.cpp:362-365      (same: openai_stream.cpp:424-426)
  state.completed = true;
  return std::vector<ProviderEvent>{
      ProviderCompleted{.response = state.response},   // copy #1
  };

src/runtime/worker_publish.cpp:82
  completed_response = completed->response;            // copy #2
```

The stream is terminal at this point — `state.response` is never read
again — and the `ProviderEvent` vector is owned by the caller, which
iterates it by `const&` (`src/runtime/worker.cpp:304-319`).

**Resolution.** `std::move(state.response)` into `ProviderCompleted`, and
have `consume_sse_events`/`publish_provider_event` consume the event vector
by value so the completion can be moved out. (A sink-style provider API —
§4.2 — subsumes this.)

**Hypothetical gain.** Two deep copies of the entire response (all streamed
text plus all tool-call arguments) per attempt removed.

### 2.4 `send()` deep-copies the Conversation history

```
src/runtime/harness.cpp:157-158
  auto messages = conversation->messages;   // full history copy per send
  messages.push_back(user_message(text));
```

The snapshot itself is contractual (API-011/012) — but *value semantics*,
not *deep copies*, are what the contract needs. Committed messages are
immutable after commit: nothing mutates a `Message` once it enters
`ConversationState::messages`.

**Resolution.** Represent history as
`std::vector<std::shared_ptr<const Message>>` (or an equivalent COW/persistent
structure). A snapshot is then a vector of refcount bumps; the machine's
`request_.messages`, the exchange, and the committed Conversation all share
one immutable copy of each message. This composes with §2.1 (the shared
request) and §2.2 (commit moves become pointer pushes).

**Hypothetical gain.** `send()` drops from `O(history bytes)` to
`O(history count × 16 B)`. For a 100-message, 100 KB conversation: ~100 KB
of copying and a few hundred allocations per send become ~1.6 KB of pointer
copies. Steady-state resident usage roughly halves for busy multi-turn
apps because §2.5's duplication disappears.

### 2.5 Tool-round messages are retained twice inside the machine

```
src/machine/turn_machine.cpp:266-270
  auto assistant = std::move(awaiting->assistant);
  request_.messages.push_back(assistant);     // copy — retained in request_
  request_.messages.push_back(results);       // copy — retained in request_
  exchange_.push_back(std::move(assistant));  // retained again in exchange_
  exchange_.push_back(std::move(results));
```

Every tool round's assistant message and results are held twice for the
rest of the turn (`request_` for resends, `exchange_` for the commit), and
tool-call arguments additionally live in the pending-call table and the
published event (§4.4). Shared immutable messages (§2.4) collapse this to
one copy plus pointers.

---

## 3. Request encoding: generic DOM plus accidental lvalue deep copies

### 3.1 Whole-request DOM subtrees are copied by lvalue assignment

The builders assemble a `glz::generic_u64` DOM and then assign **lvalues**
into parent nodes, which deep-copies the entire subtree; the largest one is
the full message array — i.e. the whole encoded conversation:

```
src/provider/anthropic_request.cpp:84    value["content"] = content;      // per message
src/provider/anthropic_request.cpp:188   root["messages"] = *messages;    // entire history DOM
src/provider/anthropic_request.cpp:196   root["tools"] = *tools;          // all schemas

src/provider/openai_request.cpp:58       function["arguments"] = *encoded;
src/provider/openai_request.cpp:78       value["content"] = *content;
src/provider/openai_request.cpp:106,147  value["content"] = text;
src/provider/openai_request.cpp:150      value["tool_calls"] = calls;
src/provider/openai_request.cpp:266      root["messages"] = *messages;
```

A generic-DOM copy is far more expensive than the JSON text itself: every
object node is a map with per-member nodes, so the duplicated tree costs
roughly 2–4× the serialized size in allocations and bytes.

**Resolution (mechanical, immediate).** `std::move(*messages)`,
`std::move(content)`, `std::move(calls)`, etc. The adjacent code already
moves in most other places (`value["input"] = std::move(*input)`), so these
read as oversights rather than design.

**Resolution (structural, larger win).** Skip the DOM for requests
entirely. The request shape is static; the only dynamic JSON is embedded
tool arguments/results/schemas, which are **already canonical JSON text**
(§3.2). Serialize directly into one `std::string` (reserve an estimate from
`message_payload_bytes`) using glaze's typed structs with a raw-JSON
passthrough type for the embedded payloads, or a small hand-rolled writer.
This removes: the DOM build (one node per field/block), the DOM copy, the
`encode_boundary_json` re-parse of every argument/result/schema
(`anthropic_request.cpp:16-19,29,43,104`), and the DOM teardown.

**Hypothetical gain.** Per attempt, for a 100 KB history: eliminates
~200–400 KB of transient DOM plus its duplicate, several thousand map-node
allocations, and a full parse of every embedded JSON payload. Request-build
time should drop by an integer factor; peak transient memory per attempt
drops from roughly `3–5× body size` to `~1.2× body size`.

### 3.2 Tool-argument JSON is parsed and re-serialized at least three times

One tool call's arguments currently pay:

1. **Provider canonicalization** at `content_block_stop` / stream finalize —
   parse + write (`src/provider/anthropic_stream.cpp:299-318`,
   `src/provider/openai_content.cpp:129-142`).
2. **Machine re-canonicalization** in `validated_call` — parse + write again
   (`src/machine/turn_machine.cpp:56-64`, via
   `canonicalize_json_object`, `src/core/json_codec.cpp:54-64`).
3. **Request re-encode** — parse again into the DOM on *every subsequent
   attempt* (`encode_boundary_json`), plus the write inside body
   serialization.

Tool results pay the same triple toll (`dispatch_tool_handler` →
`canonicalize_json` at `src/runtime/tool_dispatch.cpp:58`, then
`encode_tool_result`'s parse + write + string re-embed at
`src/provider/anthropic_request.cpp:42-59`). Persistence
(`conversation_persistence.cpp:293-312`) parses them yet again, though that
path is cold.

**Resolution.** Canonicalize exactly once at the boundary where the bytes
enter Scry (provider stream for arguments, dispatch for results,
registration for schemas — the last already happens,
`src/runtime/tool_registry.cpp:35-40`) and record that fact in the type: a
`CanonicalJson` wrapper (or a `bool canonical` on `Json`) that
`validated_call` trusts and the request encoder embeds as raw text without
re-parsing.

**Hypothetical gain.** Two parse+serialize cycles per tool payload per
round removed, and one parse per payload per retry attempt. For a turn with
4 KB of tool traffic and 4 attempts, roughly 10–20 full JSON round trips of
that payload disappear.

### 3.3 Tool schemas are copied per send and re-encoded per attempt

Schemas are immutable after registration, yet `snapshot_schemas`
(`src/runtime/tool_registry.cpp:75-86`) deep-copies name, description, and
schema text per `send()`; §2.1 then copies them per attempt; §3.1 re-parses
them into DOM per attempt.

**Resolution.** Snapshot as `std::shared_ptr<const ToolSchema>` (the
registry already stores `shared_ptr<const RegisteredTool>` — the pieces
exist), and cache the provider-side encoded form (per dialect) on the
registration, since it never changes.

**Hypothetical gain.** Tool-schema cost per attempt goes to ~0. For agents
with 10–20 tools and multi-KB schemas this is comparable to the history
cost in §2.1.

---

## 4. Streaming delta path (highest frequency)

For each `content_block_delta` today:

| Step | Cost |
|---|---|
| SSE line → `SseEvent` (`sse.cpp:133-141`) | 1–2 string allocs (name + data; `data_` capacity surrendered by move each event) |
| `parse_wire_json` of the event (`anthropic_stream.cpp:452`) | full generic-DOM parse: ~10–25 node/map allocs for a small object |
| `ProviderTextDelta{std::string{*text}}` (`anthropic_stream.cpp:231-233`) | 1 alloc (inherent) + 1 vector alloc for the single-element `ProviderEvent` vector |
| `ModelTextDelta{.text = text->text}` (`worker_publish.cpp:67`) | **1 avoidable copy** |
| `TransitionResult.commands` vector (`turn_machine.cpp:16-20`) | 1 vector alloc per delta |
| `TextDeltaEvent{.text = delta->text}` (`worker_publish.cpp:144`) | **1 avoidable copy** |
| Queue push + coalesce (`queue.cpp:9-48`) | amortized append (good) |

So ~6–8 allocations plus a DOM parse per delta, of which ~2 allocations are
inherent (the delta string itself and its queue slot; coalescing often
removes even the slot).

**Resolutions, in increasing ambition:**

1. **Move the two avoidable copies** (rvalue overloads of
   `publish_provider_event`/`publish_command`; consume the provider event
   vector by value). Mechanical.
2. **Typed event decoding.** Known stream-event shapes decoded with glaze's
   compile-time typed structs (or a minimal two-field pull parse of
   `type` + `delta.text`) instead of the generic DOM. The generic DOM
   remains a fine choice for the *request-id/error* cold paths.
3. **Sink-style provider seam.**
   `parse_stream_event(name, data, state, sink)` pushing events into a
   caller-supplied callback or reused scratch buffer removes the
   vector-per-event allocation; the same shape fits `SseParser::push`
   (append into a caller-owned `std::vector<SseEvent>&` that the worker
   clears and reuses), and `TransitionResult` can use a small-buffer vector
   (`boost::container::small_vector<MachineCommand, 2>` or C++26
   `std::inplace_vector` where bounded) since >90 % of transitions emit 0–1
   commands.
4. **View-based deltas (creative, bigger contract change).** The worker
   consumes SSE events synchronously within `consume_stream_chunk`; SSE
   `data` could be exposed as `std::string_view` into the parser's buffer,
   valid for the callback only — mirroring the public API's own borrowed
   `string_view` delta contract (API-013). Combined with (2)+(3), a text
   delta's *only* allocations become the accumulation append and the queue
   slot.

**Additional parser detail:** `line_ending_at`
(`src/protocol/sse.cpp:22-27`) scans the buffer twice (`find('\n')` +
`find('\r')`); a single `find_first_of("\r\n", offset)` halves the scan.
And because `dispatch` *moves* `data_` out, the parser reallocates the data
buffer from zero capacity for every event; swapping with a pooled string or
copying small payloads out of a retained buffer keeps capacity across
events.

**Hypothetical gain.** For a 1,500-delta stream: from ~10–35k allocations
down to ~3–5k (tier 1–3), with tier-2 removing the per-delta DOM parse that
likely dominates decode CPU. Given deltas arrive on the worker thread while
the app thread polls, this directly reduces contention on the global
allocator shared by both threads.

### 4.4 Tool-call blocks are held in up to five places at once

During a tool round the same `ToolCallBlock` (id + name + argument text)
exists in: the assistant message (`request_`/`exchange_`), the machine's
pending-call table (`turn_machine.cpp:325-333`), the published
`PublishToolCall`→ `ToolCallEvent` (copied again at
`worker_publish.cpp:153-160` when not batched), the route's
`pending_worker_tool_` (`pump.cpp:227`), and the `ExecuteWorkerToolCommand`
(`pump.cpp:228-231`). The observer notification copies id/name/arguments
once more into the public `ToolCall` (`pump.cpp:248-255`).

**Resolution.** `std::shared_ptr<const ToolCallBlock>` for the internal
hops (machine table, events, route, worker command); the public `ToolCall`
copy at the API boundary stays, honoring the borrowed-argument contract.

**Hypothetical gain.** For large-argument tools (the 1 MB default cap,
`ToolLoopPolicy::max_argument_bytes`), peak retention per call drops from
up to ~5× arguments to ~1× + pointers, and 3–4 large string copies per
call disappear.

---

## 5. `UniqueFunction`: no small-buffer optimization

```
include/scry/unique_function.hpp:23-25
  UniqueFunction(Callable&& callable)
      : object_(new std::decay_t<Callable>(...))   // heap alloc, always
```

Every callable heap-allocates, including captureless lambdas. Per turn this
is: 5 callback registrations, 1 `BodyChunkSink` per attempt
(`src/runtime/worker.cpp:277-280`, a 3-pointer capture), plus every
`ToolHandler` (which is additionally wrapped in a `shared_ptr`,
`src/runtime/state.hpp:25` / `tool_registry.cpp:68`) and the `PumpClock`.
The `BodyChunkSink` is also *invoked* once per network chunk through two
indirections (object pointer + function pointer), so locality matters.

ARCHITECTURE.md (line 427) already anticipates replacing this with
`std::move_only_function` "after ABI and allocation benchmarks" — this
report is the allocation half of that argument.

**Resolution.** Add 2–3-pointer inline storage for nothrow-move-constructible
callables (all of Scry's internal captures qualify), or adopt
`std::move_only_function` where the standard library provides SBO. Folding
`destroy_` into a single manager function (or static descriptor) also
shrinks the object from 3 words to 2.

**Hypothetical gain.** ~6–10 heap allocations per turn removed; one less
cache miss per body-chunk callback; smaller `TurnRoute` (five callbacks
inline).

---

## 6. Transport: per-attempt handle construction defeats connection reuse

```
src/transport/curl_transport.cpp:468, 485
  EasyHandle easy{curl_easy_init()};    // per attempt
  MultiTransfer multi;                  // curl_multi_init() per attempt
```

Every attempt constructs and destroys a fresh easy handle *and* a fresh
multi handle. libcurl's connection cache lives on the multi handle, so no
TCP connection or TLS session ever survives an attempt: every retry, every
tool round, and every turn pays a fresh TCP + TLS handshake, plus tens of
KB of libcurl-internal allocation per attempt.

**Resolution.** The transport is only used by the single worker thread.
Keep one `CURLM*` (and optionally one reusable `CURL*` with
`curl_easy_reset` between attempts) in `CurlTransport::Impl` for the
harness's lifetime. Handles remain single-threaded; shutdown semantics are
unchanged (the progress callback still aborts).

**Hypothetical gain.** The largest *wall-clock* item in this report:
1–2 network round trips (TCP+TLS, typically 50–300 ms against remote APIs)
saved per attempt after the first, which directly compounds across tool
rounds — a 3-round agent turn saves 3 handshakes. Allocation-wise, tens of
KB of libcurl setup/teardown churn per attempt disappear. Response-header
strings are also allocated for *every* header received
(`transport_policy.cpp:101-120`) though only request-id / content-length /
retry-after are ever read; filtering at record time is a small adjacent
win.

---

## 7. Smaller findings

| # | Finding | Location | Resolution | Gain |
|---|---|---|---|---|
| 7.1 | `bytes_by_turn_[turn_id]` default-inserts an entry even when the push is rejected or the turn is unknown; zero-byte entries linger until a matching `discard`/`release` | `src/runtime/queue.cpp:38,67,91` | `find()` first; insert only on successful push | Bounds a slow map growth in pathological reject loops |
| 7.2 | `pending_callbacks_` is scanned linearly by `deliver_one`, `has_deliverable`, `coalesce_pending_delta`, `clean_routes`, and erased from the middle | `src/runtime/pump_state.cpp:155-168, 228-263` | Per-route FIFO index, or partition by deliverability; only matters with many concurrent detached turns | O(n²)→O(n) per `update()` under backlog |
| 7.3 | Pump ingest takes the queue mutex twice per event (`size()` then `try_pop()`) | `src/runtime/pump_state.cpp:114-126` | Batch-pop (swap the deque under one lock) or loop on `try_pop` alone | Halves lock traffic on the app thread's ingest path |
| 7.4 | `completion_text` / `response_text` append without `reserve` | `src/runtime/pump.cpp:35-47`, `src/runtime/state.cpp:5-13` | Sum block sizes, reserve once | Avoids log₂(n) reallocs on large completions |
| 7.5 | Reflection codec builds a fresh `path` string per member/element visited, even on success | `include/scry/detail/reflection_codec.hpp:36-49` (used throughout `decode`/`append_encoded`) | Lazy path materialization (segment chain, stringified only inside `codec_error`) | N-field struct decode: N allocations → 0 on the happy path |
| 7.6 | `ToolHandler` takes `Json` by value; dispatch passes an lvalue, copying the full argument text per invocation | `include/scry/tool_registry.hpp:19`, `src/runtime/tool_dispatch.cpp:43` | Pass `const Json&` (or move a purpose-built copy) | One argument-sized string copy per tool call |
| 7.7 | `validate_response` copies each `ToolCallBlock` (`auto normalized = call;`) before canonicalizing, and collects `response_ids` as owned strings | `src/machine/turn_machine.cpp:39-65, 411-440` | Build the normalized call from moved parts; use `string_view` ids into the response | One arguments-sized copy + id copies per call |
| 7.8 | `SseParser::push` accumulates results through per-call temporary vectors returned by `process_complete_lines` | `src/protocol/sse.cpp:44-72, 143-167` | Append into one caller-visible vector (or sink) | 1–2 vector allocs per network chunk |
| 7.9 | Errors are large (3 strings + optionals) and copied in several cold spots, e.g. `finish_error(waiting->last_error)` on elapsed-deadline wake | `src/machine/turn_machine.cpp:218` | Move where the state is being destroyed anyway | Minor; cold path |

---

## 8. Worked estimate of aggregate gains

Scenario: 100 KB committed history, 4 KB new user text, 2 tool rounds
(4 KB combined arguments/results), 1 retry (⇒ 4 attempts total),
1,500 text deltas (~30 KB streamed text), 15 registered tools (~8 KB
schema text).

**Bytes memcpy'd today (approximate):**

| Source | Cost |
|---|---|
| `send()` history snapshot (§2.4) | 100 KB |
| Request copy per attempt (§2.1): 4 × ~110 KB | 440 KB |
| DOM encode + lvalue DOM copy per attempt (§3.1): 4 × ~2 × 110 KB-equivalent | ~880 KB in node-sized pieces |
| Response copies at stream end (§2.3): 2 × ~34 KB | 68 KB |
| Exchange copies at commit (§2.2): 2 × ~40 KB | 80 KB |
| Delta double-copy (§4): 2 × 30 KB | 60 KB |
| **Total redundant** | **≈ 1.6 MB per turn** |

**After the resolutions:** one serialize of the body per attempt
(~440 KB total, unavoidable while each attempt re-sends the body) plus the
inherent single accumulation of streamed content — roughly **0.45 MB**, a
~3.5× reduction in bytes copied, with peak transient memory per attempt
dropping from ~3–5× body size to ~1.2×.

**Allocations:** ~20–40k per such turn today (dominated by per-delta DOM
parses and DOM node churn), reducible to ~3–5k. On the wall clock, the
transport handle reuse (§6) likely dwarfs all of the above for
remote-provider deployments; the allocation work dominates for local
low-latency servers (the Ollama-class targets in the README), where
per-delta costs are the frame-budget threat for the "can't block a frame"
use case.

---

## 9. Verification plan (recommended before/after instrumentation)

1. **Allocation-counting harness:** the test suite already injects fake
   transports; add a test-only global `operator new/delete` counter and
   assert allocation ceilings for (a) one streamed delta, (b) one tool
   round, (c) one `send()` with an N-message history. This turns every
   figure in §8 into a regression gate, in the spirit of the project's
   absolute quality gates (ADR 0011).
2. **Benchmark legs:** a micro-benchmark target (nightly, not the fast
   gate) driving the fake transport with a recorded 2k-delta SSE corpus —
   wall time + peak RSS before/after each tier.
3. **Heap profiling spot-check:** one run under `heaptrack`/`massif` on the
   showcase example to confirm the DOM encode and per-attempt request copy
   are the top allocators, as this reading predicts.

---

## 10. Prioritized roadmap

| Tier | Items | Character | Est. effort |
|---|---|---|---|
| 1 — mechanical moves | §3.1 `std::move` into DOM nodes; §2.2/§2.3/§4.1 rvalue overloads + `std::move(state.response)`; §7.6; §7.9 | No semantic change, localized diffs | Hours |
| 2 — ownership restructuring | §2.1 request-by-reference/shared; §3.3 shared schema snapshots; §6 persistent curl multi handle; §5 SBO or `std::move_only_function` | Internal seams only; contracts intact | Days |
| 3 — codec strategy | §3.2 canonical-once JSON type; §4.2 typed stream-event decode; §3.1 direct request serialization | Removes the generic DOM from all hot paths | ~1 week |
| 4 — structural sharing | §2.4/§2.5 shared immutable `Message` history; §4.4 shared tool-call blocks; §4.3-4 sink-style provider/SSE seams | Largest resident-memory win; touches many files | 1–2 weeks |

**What is already good and should be preserved:** the per-turn byte
accounting (`event_payload_bytes` + budgets) that makes a stalled pump
bound memory, not just rate; producer- and consumer-side delta coalescing
(`queue.cpp:9-26`, `pump_state.cpp:155-168`); the terminal-event byte
reserve (`worker_publish.cpp:15`); single-erase-per-push SSE buffering; and
the generally correct use of moves in single-owner handoffs — the findings
above are concentrated at a handful of seams, not spread through the code.
