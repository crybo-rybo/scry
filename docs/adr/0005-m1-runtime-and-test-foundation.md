# ADR 0005: M1 Runtime and Test Foundation

- Status: Accepted
- Date: 2026-07-17

## Context

M1 introduces the first compiled runtime, pure behavioral suites, libcurl as a
production dependency, and Glaze-backed provider translation. ADR 0003 deferred
the test-framework choice until those tests existed. M1 also needs stable
internal value contracts so the machine, provider, transport, and actor work
can proceed independently without making implementation types public.

## Decision

- Build `scry::scry` as a compiled C++23 library and keep every third-party
  header behind its private source boundary.
- Use the system libcurl with a 7.84 minimum. Package metadata discovers curl
  for static consumers.
- Fetch Glaze at commit `8b60d82c66311c145c4d03be3b556b555a9cb111`
  (the `v7.5.0` release) for runtime JSON translation and the reflection spike.
- Ratify Catch2 v3 at commit
  `dfc2dff8d70d083c60c1c6986030e5389a867a93` (the `v3.9.1` release).
  Catch2 is test-only, excluded from installation and package exports, and
  compiled under the same C++23 project settings.
- Preserve the small framework-free public contract executable.
- Keep neutral messages, transport values, and provider events as internal
  Scry-owned values. The provider adapter and transport remain the only two
  virtual seams.
- Keep M1 chat-only. Tool-capable value shapes are present so M2 extends the
  contracts, but M1 does not dispatch tools or enter a tool-await state.

## Consequences

Core M1 builds now acquire Glaze source on first configure and require libcurl
development files. Consumers never fetch Catch2 or Glaze. Parallel feature work
must propose shared-contract changes through the integration owner rather than
introducing alternate types or compatibility layers.
