# ADR 0001 — Build system: CMake ≥ 3.25 with presets, `just` as the command front-end

**Status:** accepted · **Date:** 2026-07-17

## Context

The docs never named a build system. Requirements it must satisfy: a four-leg
compiler matrix including experimental toolchains (PORT-001/002/005), a
severable feature flag for the reflection layer (TOOL-003), sanitizer build
modes as first-class citizens (QA-005), and `just ci-fast` running everything
CI enforces (QA-011).

## Decision

- **CMake ≥ 3.25** with **CMakePresets.json** as the single source of build
  configurations. Every build mode (dev, release, asan-ubsan, tsan,
  reflection) is a named preset; CI and local commands invoke the same
  presets, so the two cannot drift.
- **Ninja** as the generator.
- **`just`** as the thin command front-end (`just ci-fast`, `just tidy`, …).
  Recipes only compose preset invocations and lint commands; no build logic
  lives in the justfile.

## Alternatives considered

- **Bazel** — hermetic and matrix-friendly, but hostile to experimental
  compilers (custom toolchain definitions per fork) and a heavy ongoing cost
  for a solo project.
- **Meson** — competent, but the dependency stack (Catch2, Glaze, curl) and
  the IDE/tooling ecosystem integrate with CMake natively; no offsetting
  advantage here.

## Consequences

- `CMAKE_EXPORT_COMPILE_COMMANDS` is on in the base preset, so clang-tidy and
  IDEs get an accurate compile database for free.
- Preset names are load-bearing: CI, justfile, and docs reference them;
  renaming one is a breaking change to all three.
