# ADR 0004 — Reflection toolchain CI strategy

**Status:** accepted · **Date:** 2026-07-17

## Context

PORT-002 requires the reflection layer to build on GCC trunk and
clang-p2996; PORT-005 (as amended 2026-07) requires reflection-ON CI legs on
Linux from M0, with macOS reflection legs deferred to the evolution register.
Practical availability differs by compiler: Bloomberg's
[clang-p2996](https://github.com/bloomberg/clang-p2996) has a community
prebuilt image ([vsavkov/clang-p2996](https://hub.docker.com/r/vsavkov/clang-p2996));
GCC reflection support is on trunk only — building from source, no prebuilt
trunk images. Reflection facilities in clang-p2996 (P2996 plus P3394
annotations) sit behind `-freflection-latest`; GCC trunk enables them in
C++26 mode.

## Decision

Phased, honesty-first:

1. **Now (M0 scaffold):** a **non-gating** `reflection-probe` CI leg builds
   the library with `SCRY_ENABLE_REFLECTION=ON` in the `vsavkov/clang-p2996`
   container. Non-gating because a community image we don't control must not
   be able to hold per-commit CI hostage; visible because the toolchain being
   the long pole is exactly what we want early warning on.
2. **Spike A (P2996 schema generation):** pins the toolchain properly —
   either a digest-pinned image mirrored to a registry we control, or a
   cached from-source build — and adds the **GCC trunk leg**. Both reflection
   legs become **gating** in that PR.
3. **macOS reflection legs:** deferred with a trigger in the evolution
   register (ARCHITECTURE.md §11): when a prebuilt or cheaply-cached P2996
   toolchain for macOS exists, the matrix goes to full reflection ON/OFF on
   both platforms.

## Consequences

- Until Spike A lands, reflection breakage surfaces as a visible-but-yellow
  probe, not a red build — an explicit, documented exception to "red means
  stop" with a scheduled end.
- The severability guarantee (TOOL-003) is carried by the *gating*
  reflection-OFF legs from day one.
