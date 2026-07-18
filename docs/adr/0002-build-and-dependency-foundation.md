# ADR 0002: Build and Dependency Foundation

- Status: Accepted
- Date: 2026-07-17

## Context

Scry needs stable C++23 builds, an isolated experimental C++26 reflection leg,
sanitizers, downstream package consumption, and one local command equivalent to
CI. The M0 probes also need Glaze source built with experimental compiler flags
and a system libcurl new enough for thread-safe global initialization.

## Decision

- Use CMake 3.25 or newer with Ninja and checked-in configure presets. `just`
  remains a thin command front end; build logic stays in CMake.
- Pair Linux Clang with libc++. The hosted Clang/libstdc++ combination does not
  expose the required C++23 `std::expected`; GCC continues to use libstdc++.
- Install and export `scry::scry`, then compile a downstream `find_package`
  consumer in local CI to keep package metadata honest.
- Acquire libcurl from the system with a 7.84 minimum. Fetch Glaze only for the
  reflection spike and pin its exact release tag.
- Use plain CTest executables for the compile-only M0 contracts. Select a test
  framework when behavioral suites in M1 make its assertion and generator
  facilities valuable, rather than adding a dependency preemptively.
- Pin lizard as a CI-only complexity checker; it has no effect on consumers or
  runtime dependencies.

## Consequences

Core builds remain network-independent and reflection-independent. The
reflection probe needs network access on first configure, while the curl probe
needs system development headers. A future source dependency or test framework
requires its own written justification under PORT-003.
