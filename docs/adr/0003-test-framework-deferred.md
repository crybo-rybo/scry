# ADR 0003: Test Framework Deferred to M1 (leading candidate: Catch2 v3)

- Status: Accepted — framework selection deferred until M1 behavioral suites land
- Date: 2026-07-17

## Context

M0 is compile-only: the contract suite is static_asserts plus a small
plain-assert executable, which needs no framework. Pulling a framework in now
would add a dependency (PORT-003 makes those gated decisions) months before
behavioral tests exist to justify it. The M1 testing plan (ENGINEERING.md §2)
does want framework affordances: sections/fixtures for machine tests,
generators for property-style inputs (randomized SSE chunk splits), and CTest
registration across the CI matrix.

## Decision

Defer the dependency until the first M1 behavioral suite needs it. Analysis
preserved for that decision:

- **Catch2 v3 (leading candidate)** — `SECTION`s fit machine tests; built-in
  `GENERATE` covers light property-style testing without a second dependency;
  `catch_discover_tests` handles CTest. Pin via FetchContent to an exact tag,
  declared `SYSTEM`, built with the project-wide C++ standard (a standard
  mismatch produces link errors, e.g. Catch2's `StringMaker<string_view>`).
- **GoogleTest** — its differentiator is GMock, which this project explicitly
  rejects ("fakes over mocks", ENGINEERING.md §2); otherwise no advantage.
- **doctest** — fastest compiles, stagnant maintenance, weaker generators.
- **Boost.Test** — pulls Boost into a two-runtime-dependency project.

## Consequences

- M0 contract tests stay framework-free; whatever lands at M1 must absorb
  them or leave them as-is (they are cheap either way).
- The framework never appears in public headers (API-002) and is a
  build-time-only dependency.
