# Source Documentation {#source_documentation}

This view is a guided map of Scry's maintained implementation. It covers every C++ header and
source file under `src/`; the source browser also carries the installed and component headers from
`include/` so declarations can be followed across the package boundary. Tests, benchmarks,
examples, and build tooling are deliberately outside the generated code index.

@htmlonly
<div class="scry-note scry-note-source">
  <strong>Contributor contract.</strong> Internal documentation explains ownership, invariants,
  failure behavior, and why a seam exists. It is not a compatibility promise. Start with the
  requirements and load-bearing design documents before changing behavior.
</div>
@endhtmlonly

## Architecture map

| Area | Start here | What belongs here |
|---|---|---|
| Core contracts | @ref scry::detail::ModelRequest "ModelRequest", @ref scry::detail::ProviderAdapter "ProviderAdapter", @ref scry::detail::Transport "Transport" | Provider-neutral messages, error construction, JSON boundaries, retry policy, logging, and the narrow provider/transport seams. |
| Turn machine | @ref scry::detail::TurnMachine "TurnMachine" | The deterministic sans-I/O state machine: events in, commands out, explicit phase and diagnostic handling. |
| Protocol parsing | @ref scry::detail::SseParser "SseParser" | Incremental, bounded Server-Sent Events parsing independent of transport and provider semantics. |
| Provider adapters | @ref scry::detail::AnthropicAdapter "AnthropicAdapter", @ref scry::detail::OpenAiAdapter "OpenAiAdapter" | Request encoding and streamed-event decoding for each supported dialect, lowered to neutral model values. |
| Runtime and pump | @ref scry::detail::WorkerActor "WorkerActor", @ref scry::detail::PumpState "PumpState", @ref scry::detail::TurnRoute "TurnRoute" | Actor ownership, command/event routing, callback delivery, conversation state, and tool dispatch. |
| Queues and bounds | @ref scry::detail::BlockingQueue "BlockingQueue", @ref scry::detail::EventQueue "EventQueue" | The two cross-thread message channels and their explicit admission and payload bounds. |
| Transport | @ref scry::detail::CurlTransport "CurlTransport", @ref scry::detail::transport_policy::ResponseState "ResponseState" | libcurl lifetime, request validation, streaming callbacks, cancellation, and provider-error hygiene. |
| Reflection bridge | @ref scry::reflection::detail::JsonView "JsonView" | The compiled JSON bridge beneath the optional C++26 schema, codec, and registration templates. |

## Follow one turn through the code

1. Public @ref scry::Harness::send "Harness::send()" validates admission and creates the
   pump-side route and worker command.
2. @ref scry::detail::WorkerActor "WorkerActor" owns network and loop mutation on its thread.
3. @ref scry::detail::TurnMachine "TurnMachine" translates provider-neutral events into explicit
   commands without performing I/O.
4. A @ref scry::detail::ProviderAdapter "ProviderAdapter" encodes requests and decodes streamed
   protocol data; @ref scry::detail::Transport "Transport" owns only the HTTP boundary.
5. @ref scry::detail::PumpState "PumpState" drains worker events inside
   @ref scry::Harness::update "Harness::update()", invokes callbacks and tools, and commits a
   successful exchange.

This path is a useful first review order because it makes the dependency direction visible:
runtime orchestration depends on neutral core contracts; adapters depend on those contracts;
transport does not know conversation or tool semantics.

## Browse and investigate

@htmlonly
<div class="scry-index-links">
  <a href="files.html"><strong>File index</strong><span>Every maintained header and source in <code>include/</code> and <code>src/</code></span></a>
  <a href="annotated.html"><strong>Type index</strong><span>Public and internal classes, structs, and unions</span></a>
  <a href="namespaces.html"><strong>Namespace index</strong><span>The public namespace and implementation detail layers</span></a>
</div>
@endhtmlonly

Each file page includes its full source, include relationships, and links between referenced
symbols. Header-declared private members are included because they explain implementation
contracts. Translation-unit-local helpers remain visible in browsable source without being
promoted into standalone reference entities.

## Documentation boundaries

- `include/` is the consumer-facing and optional component header surface. The API view curates
  the supported entry points; `detail/` remains an implementation namespace even when its source
  is inspectable.
- `src/` is contributor-facing implementation documentation. Its comments should emphasize
  invariants, ownership, thread affinity, bounds, and error translation rather than restating C++.
- `docs/requirements.md` is normative. `docs/design/` defines product behavior and public
  concepts; `docs/architecture/` owns boundaries and dependency direction.
- Unit-test seams, fixtures, benchmarks, and examples are intentionally not separate Doxygen
  input trees. Their behavior belongs in the test suite and development documentation.
