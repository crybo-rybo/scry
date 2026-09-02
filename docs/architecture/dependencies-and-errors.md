# Architecture: Errors and Dependencies

## 8. Errors as Values, Categorized Once

- Internal fallible paths return `std::expected<T, Error>`; `Error` is one struct with a category enum (`invalid_config`, `invalid_state`, `invalid_argument`, `busy`, `authentication`, `rate_limit`, `network`, `protocol`, `resource_limit`, `tool`, `max_tool_rounds`, `cancelled`) plus message, sanitized provider detail, HTTP status, retryability, and correlation fields. One error type end-to-end — no per-layer error hierarchies to translate between.
- `Error` remains a designated-initializer-friendly aggregate. Its category,
  retryability flag, HTTP status, and attempt count form a compact scalar header
  before the diagnostic and correlation values; designated initializers that set those
  fields follow that declaration order. Keeping the scalars together avoids
  carrying two separate padding gaps through `expected` values and event
  queues.
- The retry classifier (which categories are retryable) is a pure function owned by the loop state machine, tested as a table.
- At the boundary, failures before work is accepted are returned immediately by `std::expected`. After acceptance, the same `Error` arrives through `on_finished` in place of the completion — including cancellation, as category `cancelled`. `errno`-style status polling is deliberately absent; there is exactly one asynchronous outcome channel.

## 9. JSON and Dependency Policy

- **Glaze** for JSON (header-only, fast, and aligned with the pinned
  reflection toolchain); treated as an *internal* dependency — no Glaze header
  or type appears in Scry's public include path. The stable tool boundary uses
  the Scry-owned `Json` value, and the optional reflection component exposes a
  Scry-owned JSON-view bridge whose implementation alone includes Glaze. A
  downstream core or reflection consumer never discovers or links an exported
  Glaze target.
- **One codec, one canonical form.** A single internal JSON codec serves every
  seam — provider wire encoding and decoding, the tool boundary, Conversation
  persistence, and the reflection bridge — and it emits exactly one canonical
  form, with object keys sorted lexicographically. There is no second encoder
  and no per-seam canonicalization to keep in agreement, so "canonical JSON"
  means one thing throughout the codebase.
- **Google Benchmark 1.9.5** at commit
  `192ef10025eb2c4cdd392bc502f0c852196baa48` is the pinned,
  build-only profiling framework. It provides calibrated repetitions, warm-up,
  optimizer barriers, counters, filtering, and machine-readable output rather
  than making Scry maintain a second timing framework. It is fetched only when
  `SCRY_BUILD_BENCHMARKS=ON`, which defaults to `OFF`; its targets are private
  to benchmark executables and are never installed or exported. It therefore
  does not expand the SCRY-PORT-003 runtime dependency set or the normal
  consumer graph.
- Dependency bar is high: curl, Glaze, and test frameworks. Each new dependency
  needs a written justification in this doc. Header hygiene is enforced (IWYU
  in CI) so the PImpl firewall stays real.

**Showcase boundary.**
Dependency direction is one-way: showcase code depends inward on the public
`scry::scry` surface, and the library never depends back. The ImGui panel, its
controller seam, and the NPC world live outside namespace `scry` and are
neither installed nor exported; the host owns the Harness, Conversation, update
cadence, ImGui context/backends/window/loop, and world lifetime. Their
explicit-schema handlers close over host-owned in-memory state — the sanctioned
seam for state a main loop already owns, not an engine abstraction or a second
agent loop. Dear ImGui is a build-only, default-OFF, pinned MIT dependency that
must compile the real widget rather than a look-alike facade; no ImGui header,
type, target, source, option, or transitive requirement may appear in the
public, installed, or exported package surface, so the core runtime dependency
set remains libcurl plus internal Glaze. SHOW-001–004 state the binding form.
