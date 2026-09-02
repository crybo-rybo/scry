# Architecture: Tools, Providers, and Transport

## 5. Tool Registry: Type Erasure Below, Optional Reflection Above

Two layers, one table (as settled in the
[tools design](../design/tools-and-providers.md)):

**Lower layer — type erasure.** A registered tool is a record: name, description, schema (JSON string), and a type-erased callable (`json → expected<json, error>` in spirit; Scry's `UniqueFunction` keeps captures move-only). This is the `std::function`-style erasure idiom: the registry is runtime-uniform, closed to no one, and has zero knowledge of reflection.

The Registry is owned by its Harness. `send()` snapshots its immutable shared
records for the accepted turn, so registration during `update()` never mutates
an in-flight turn and the live registry never crosses the worker boundary. Only
a frozen registry-level snapshot of neutral schemas — shared, never re-copied
per turn — crosses; every handler stays exclusively pump-owned. The
public registry cannot be moved out of its Harness, and explicit schemas are
parsed and canonicalized at registration. Mutation is additive-only: duplicate
names are rejected, and replacement/removal remain absent until a real
hot-reload contract defines their snapshot semantics.

**Execution ownership.** There is one execution policy: every handler runs on
the app thread inside `update()`. A provider batch is published atomically; the
pump then walks it in provider order, invoking each handler directly, reserving
the canonical result against the machine's authoritative remaining exchange
budget, posting the result, and only then running the tool observer before
admitting the next call. A fatal framework or cumulative-result-budget failure
latches the route and permanently suppresses the batch suffix before any later
handler can run. Nothing
about a tool call crosses the worker boundary except the resulting neutral
message content, which is why the shutdown bound in the
[runtime architecture](runtime.md) needs no carve-out for
application code. A slow handler therefore costs frame time; the intended answer
for that case is an asynchronous/deferred tool-result API (see the
[evolution register](quality-and-evolution.md)), not a second thread quietly
executing app callbacks.

**Upper layer — consteval code generation (optional, experimental).**
The P2996 layer is a compile-time *code generator* targeting the lower
layer. Given a plain aggregate, it builds
`scry::reflection::input_schema_v<Args>` in canonical fixed storage and
instantiates a typed deserializer/invoker erased into an ordinary
`ToolHandler`. `scry::reflection::add<Args>(registry, metadata, handler)` then
calls the existing public registry operation. The free function keeps C++26
reflection declarations out of the stable `ToolRegistry` class. The same
typed encoder is exposed independently as `scry::reflection::encode(value)`;
both it and reflected handler results canonicalize through the compiled
Scry-owned JSON bridge.

The dependency direction is deliberately split:

```text
app aggregate + handler
        ↓
public reflection header (P2996 + standard/Scry-owned types only)
        ↓
Scry-owned typed wrapper and JSON-view contract
        ↓
compiled optional scry::reflection component
        ↓
private Glaze-backed JSON implementation
        ↓
ToolDefinition + ToolHandler → existing ToolRegistry
```

This keeps both promises real: template instantiation can see application
types, while Glaze remains absent from public headers and installed dependency
metadata. A reflection-OFF build installs only the C++23 core and core headers;
the optional `reflection` package component owns its public/detail headers,
compiled bridge, C++26 flags, and `scry::reflection` target.

Additional practices:

- **Concepts guard the gate.** Root and nested objects are complete,
  default-initializable, non-union aggregates without bases, bit-fields,
  reference/cv members, or unsupported recursive values. Misuse fails at the
  reflected call with Scry-owned diagnostics rather than in a dependency's
  template internals. Nested optionals, scoped enum aliases, and every
  `vector<bool, Allocator>` specialization are rejected because they cannot
  preserve the accepted value semantics unambiguously.
- **One lexical schema.** Object/property keys and `required` member names are
  sorted lexicographically, enum values retain declaration order, and nested
  objects are inlined and closed. The compile-time artifact is the exact text
  passed to the lower registry; there is no runtime schema cache or macro
  registry.
- **One strict value mapping.** Schema, decode, handler-result encode, and
  direct encode share the closed recursive type matrix defined by the SCRY-TOOL
  requirements. Glaze gaining a serializer does not expand Scry's public
  contract.
- **Presence is declaration-driven.** P2996 default-member-initializer
  reflection controls omission; `std::optional` controls nullability. The
  decoder constructs normal C++ defaults and validates the canonical parsed
  object before invocation.
- **Descriptions have one source.** The P3394 `description` annotation is the
  only way to attach a tool or parameter description; there is no portable
  trait-based fallback or override to keep in agreement with it.
- **Canonical parsed input is the seam.** Unknown/missing/type/range checks run
  on the canonical unique-key object. Detecting duplicate lexical JSON keys
  would require a different parser boundary and is deliberately out of scope.

## 6. Provider Adapters: Strategy at a Narrow Seam

One of the two sanctioned virtual interfaces. The pattern is classic
**Strategy**: a small internal interface — translate neutral request → wire
request, parse wire stream events → neutral events. Anthropic and the documented
OpenAI-compatible Chat Completions subset are implemented on the same seam.
Adapters receive a `Config` that `Harness::create` has already validated; they
translate, they do not re-check configuration.

Discipline that keeps it clean:

- **The neutral model is the API of this seam.** Adapters see `Message`/`ContentBlock`/`ToolSchema` and nothing above; nothing above sees JSON or HTTP. Wire-format knowledge concentrated in one file per provider. `Message`, `ContentBlock`, and their block types are the public `scry::Message` family re-exported into `scry::detail`, so the model the adapters encode is exactly the model `Conversation::messages()` hands the host; `ToolSchema` stays internal.
- **Adapters are stateless translators** where possible; stream-parsing state (partial SSE event, current content block index) is an explicit per-turn parser object, not adapter member state — one adapter instance serves many turns.
- Selection is config-driven via an internal factory keyed on dialect enum. No plugin registration machinery until a third-party provider actually needs it — **YAGNI applies to extension points too.**
- **Golden-file tests** per adapter: wire payloads hand-synthesized from the provider references, with a documented capture recipe in [`tests/fixtures/README.md`](../../tests/fixtures/README.md), asserted against neutral-model round-trips. This is the layer where upstream API drift bites, so tests are data, easy to re-capture.

The OpenAI strategy owns endpoint normalization, optional Bearer auth,
the common request envelope, one-choice response validation, function-tool
translation, usage/finish mapping, and strict `[DONE]`-terminated streaming.
Its portable baseline omits Azure shapes, Responses API, structured output,
reasoning-specific token fields, and provider extensions. The sole optional
reasoning control is the typed `Config::reasoning_mode`: the default preserves
the baseline by omitting a wire field, while `disabled` maps to OpenAI-compatible
`reasoning_effort: "none"`; unsupported endpoints remain a configuration or
deployment choice rather than an adapter capability probe. Adapter objects stay
stateless; `ProviderDecodeState` carries a dialect-specific alternative so
OpenAI chunk IDs, indexed tool fragments, finish state, and usage cannot leak
into the Anthropic lifecycle. HTTP classification and sanitized request IDs
remain transport responsibilities.

## 7. Transport: RAII-Wrapped curl Behind an Injectable Seam

The second sanctioned interface, existing for one reason: **dependency injection of a fake transport in tests** (and of the sans-I/O driver's event source). Practices:

- libcurl used directly (not through a wrapper lib) for SSE control, but every curl object lives in a RAII wrapper with the curl types visible only in the `.cpp`. curl's C callbacks trampoline into C++ via the standard `void* userdata` → object pointer pattern, with all exceptions caught at the trampoline (C stacks must never unwind).
- **SSE parsing is a pure incremental function**: bytes in, zero-or-more events
  out, remainder buffered. It performs no I/O and all retained data is covered
  by the configured event bound, making it property-testable with randomly
  split byte chunks (the classic bug in SSE parsers is
  delimiter-across-chunk; the test generator targets it directly).
- Curl's progress callback checks both the worker `stop_token` (Harness shutdown) and the active turn's atomic cancellation flag. Neither signal is repurposed for the other.
- A non-2xx response body is never delivered to the stream decoder. The transport retains at most 8 KiB of it solely to extract the provider's own error token, which reaches `Error::provider_detail` under the dialect namespace; the retained bytes are then discarded with the transfer.
- Connection reuse (curl multi/share) is an internal optimization invisible above the seam.
