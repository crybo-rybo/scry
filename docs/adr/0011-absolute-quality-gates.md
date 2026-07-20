# ADR 0011: Absolute Quality Gates Replace the Live Ratchet

- Status: Accepted
- Date: 2026-07-19
- Supersedes: [ADR 0004](0004-live-quality-ratchet.md)

## Context

An architecture review measured the quality tooling at roughly half the size
of the library it guards and identified its costliest parts:

- The merge-base ratchet built and tested the entire project twice per run
  (with `--repeat until-fail:3` on both sides plus per-binary coverage
  repeats), defending an uneditable baseline against an adversarial
  contributor who does not exist on a solo project — the sole author can edit
  the gate itself.
- Five simultaneously ratcheted debt counters (total coverage, CRAP
  violations, complexity warnings, long functions/files, unlinked TODOs)
  duplicated protection the absolute limits already provide, while creating
  spurious merge blocks.
- `reflection_coverage_gate.py` (241 lines plus a 184-line test suite)
  hard-failed unless gcovr's JSON contained *exactly one* excluded decision of
  *exactly* type `switch` — calibrated to the precise codegen of GCC 16 +
  gcovr 8.6 on one header. Every plausible failure was "update the
  validator", never "found a bug".
- The nightly Mull mutation job was non-gating by construction
  (`--allow-surviving`) and produced reports no process required anyone to
  read, at the cost of a SHA-pinned Mull/LLVM/Ubuntu triple.
- The nightly smoke pinned the pulled model's manifest digest, so every
  upstream repush broke the nightly for reasons unrelated to Scry.

ENGINEERING.md §8 already committed to demoting process weight that stops
paying for itself.

## Decision

- **Absolute gates, not ratchets.** The quality gate builds and measures only
  the candidate tree and enforces fixed floors: diff branch coverage ≥ 90% on
  changed production lines, ≥ 95% branch coverage on the turn machine, SSE
  parser, and retry classifier, total branch coverage ≥ 88% as a coarse
  backstop against erosion in files no component floor lists, no function
  with CRAP > 30, cyclomatic complexity ≤ 15, and zero unlinked TODOs (now a
  simple absolute check in `ci-local.sh`). The merge-base is used only to
  identify changed lines. The aggregate floor deliberately sits below the
  measured ~89.9% so that honest deletions of well-covered dead code do not
  block; it is raised deliberately as coverage grows, never lowered silently.
- **The debt-counter ratchets are deleted.** The top CRAP scores and the
  QA-004 warn-only list of production functions over CCN 10 remain printed on
  every run as visibility, not gates. Function/file length is a smell,
  reported by lizard locally, not a gate — resolving the prior inconsistency
  where ENGINEERING.md called length a warning while `ci-local.sh`
  hard-failed on it.
- **Coverage runs once.** Each test binary runs a single deterministic pass
  (fixed Catch2 ordering, atomic profile counters) for profile collection.
  Repeat-run flake detection (`--repeat until-fail:3`) moves to the TSan leg
  — hosted and in local preflight — where nondeterminism actually surfaces,
  instead of running the suite ~20 times per PR across legs.
- **The reflection codec is gated with stock gcovr.**
  `--fail-under-decision 85 --fail-under-function 95` on the codec header and
  the existing `--fail-under-branch 95` on the compiled bridge replace the
  bespoke exclusion validator. The decision floor sits below the ~89%
  unadjusted result because gcovr still counts the one documented
  GCC-generated switch; a coarser threshold on one header is the accepted
  price of deleting the validator and its format coupling.
- **Mutation testing is on-demand.** The Mull job runs on
  `workflow_dispatch` per milestone, not nightly.
- **The model manifest digest pin is dropped.** The Ollama binary remains
  checksum-pinned (an executable we run); the pulled model tag is not (data
  whose upstream repushes are routine). Protocol correctness is gated by the
  deterministic fake-transport suites, not the smoke.
- `cmake/CheckReflectionPackage.cmake` is deleted: it string-matched CMake's
  generated export files — not a stable format. Its core-isolation property
  (TOOL-003) is instead proven by compilation: `scripts/ci-reflection.sh`
  builds the core-only C++23 `tests/package_consumer` with non-reflection
  GCC 14 against the reflection-enabled installation, so any leaked
  `-std=c++26`/`-freflection` flag, Glaze include, or unrequested reflection
  component fails the build rather than a grep.
- The libcurl runtime spike folds into the core GCC CI leg instead of
  occupying a dedicated runner.

## Consequences

The quality job halves in wall time (one build, one suite pass), the Python
tooling and its meta-tests shrink by roughly half, and the exact-format
couplings to lizard/gcovr internals that forced version pins are reduced to
the documented gcovr 8.6 pin for decision analysis. A total-coverage
regression that stays above the 88% aggregate floor and every component floor
lands without blocking; the printed per-run summary keeps the exact number
and the CCN warning list visible, and any floor can be raised deliberately if
drift is observed. Mutation reports arrive only when requested, so their
value depends on actually being run at milestones.
