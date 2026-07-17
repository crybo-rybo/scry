# ADR 0003 — Test framework: Catch2 v3

**Status:** accepted · **Date:** 2026-07-17

## Context

The testing plan (ENGINEERING.md §2) wants: a large deterministic
machine-test suite, property-based testing where inputs are adversarial,
CTest integration for the CI matrix, and explicitly **no mocking framework**
("fakes over mocks" — the seams are few and narrow enough to fake by hand).

## Decision

**Catch2 v3** (pinned via FetchContent, ADR 0002), with `catch_discover_tests`
for CTest registration.

- `SECTION`s fit machine tests (one scenario setup, many assertion branches).
- Built-in `GENERATE` covers light property-style tests (e.g., randomized SSE
  chunk splits) without a second dependency. If the SSE/schema property tests
  outgrow it, a dedicated property-testing library gets its own PORT-003
  justification then — not preemptively.

## Alternatives considered

- **GoogleTest** — its main draw is GMock, which this project deliberately
  doesn't want; otherwise no advantage over Catch2 here.
- **doctest** — fastest compiles, but maintenance has been stagnant and the
  generator/property story is weaker.
- **Boost.Test** — pulls Boost into a two-dependency project.

## Consequences

- Catch2 is a build-time-only dependency; it must never appear in public
  headers (API-002 include audit will enforce).
- Test binaries link `Catch2::Catch2WithMain`; no hand-written `main()`.
