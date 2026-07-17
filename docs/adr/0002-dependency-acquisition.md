# ADR 0002 — Dependency acquisition: system libcurl, FetchContent for source deps

**Status:** accepted · **Date:** 2026-07-17

## Context

PORT-003 caps runtime dependencies at libcurl + Glaze; test frameworks are
allowed on top (ARCHITECTURE.md §9). The toolchains are experimental (GCC
trunk, clang-p2996), which favors building source dependencies with exactly
our flags and stdlib rather than consuming prebuilt binaries.

## Decision

- **libcurl:** consumed from the system via `find_package(CURL)` when the
  transport lands (M1), with the PORT-006 version floor (≥ 7.84.0) checked at
  configure and `CURL_VERSION_THREADSAFE` verified at first initialization.
  curl is ubiquitous and its ABI is stable; vendoring it buys nothing.
- **Source dependencies (Catch2 now; Glaze at M1/M2):** CMake
  **FetchContent**, pinned to an **exact release tag**, declared `SYSTEM` so
  third-party headers stay outside our warnings-as-errors and clang-tidy
  gates. Upgrades are deliberate, reviewed diffs (ENGINEERING.md §7 treats
  toolchain/dependency drift as first-class risk).
- Every new dependency needs a written justification committed with the
  change (PORT-003) — this file is where those justifications accrue.

## Alternatives considered

- **vcpkg / Conan** — right answer when binary dependencies multiply or
  build times hurt. Two source deps and one system lib don't clear that bar,
  and a package-manager bootstrap step would complicate the experimental
  toolchain legs. **Trigger to revisit:** a third source dependency, or
  Catch2/Glaze rebuild times measurably dragging CI.

## Consequences

- Configure requires network access on first run (FetchContent clone). CI
  runners and the clang-p2996 probe container need `git` installed.
- No lockfile beyond the pinned tags; tag pins are the audit trail.
