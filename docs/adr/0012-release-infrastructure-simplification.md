# ADR 0012: Release-Posture Infrastructure Simplification

- Status: Accepted
- Date: 2026-07-20
- Amended: 2026-08-03 — reflection suite membership follows the live M3
  contract while the retained gate remains unchanged
- Amends: [ADR 0011](0011-absolute-quality-gates.md) (retires its gating
  machinery), [ADR 0010](0010-m5-showcase-contract.md) (moves the showcase
  gate to the nightly ring)

## Context

The project's verification infrastructure was built for a specific
development mode: unattended, agent-driven implementation with no human
reviewer. In that mode, mechanical proof substitutes for review — diff branch
coverage against the merge-base, per-component coverage floors, an aggregate
backstop, per-function CRAP ceilings, coverage-exclusion justification
tokens, mutation reports, and a gcovr-thresholded reflection coverage leg.
That machinery did its job: the deterministic suites it demanded exist and
pass, and the final full run before this decision measured 93.322% diff
branch coverage and a maximum CRAP of 13.125 against a limit of 30.

Approaching the v0.0.1 release, the maintenance mode changes: a human
maintainer reads every diff. Measured against that mode, the apparatus was
out of proportion to what it guarded:

- The metrics stack was ~1,160 lines across six files — `quality-gate.sh`,
  `quality_gate.py`, `quality_metrics.py`, a 279-line unit-test suite for the
  gate itself (run at the start of every gating invocation), and a bespoke
  C++ sub-project (`quality_coverage_mapper`) existing only to give llvm-cov
  a stable whole-archive mapping — roughly 60% of `scripts/` by line count.
- Every pull request ran ten runner jobs across six pinned toolchains
  (clang-format-18, GCC 14, Clang 18 + libc++, AppleClang, Clang 21 from the
  LLVM apt repository for fuzzing, GCC 16 from a PPA for reflection), each
  pin an individually justified but collectively large rot surface.
- The per-PR ring gated on demo code (the showcase job) against
  ENGINEERING.md §8's own judgment that example apps deserve pragmatism.
- Two feasibility spikes (`spikes/curl_sse.cpp`, `spikes/reflection_glaze.cpp`)
  outlived the questions they answered — the shipped Curl transport and
  reflection component carry their own test suites — yet the curl spike still
  built and ran on the core GCC leg of every PR.
- The mutation job had already been demoted to `workflow_dispatch`-only by
  ADR 0011 because its non-gating reports had no required reader; the
  remaining cost was carrying Mull's pinned toolchain triple and 230 lines of
  script/config for a job nobody dispatched.

ENGINEERING.md §8 commits to ratcheting process weight downward when a gate
stops paying for itself. ADR 0011 exercised that clause once; this decision
exercises it again, at the boundary where the project's development mode
changes.

## Decision

**Keep the physics, delete the actuary.** Gates that observe behavior stay;
apparatus that scores metrics goes.

Retained, unchanged:

- The three-leg core matrix (Linux GCC 14, Linux Clang 18 + libc++, macOS
  AppleClang) with warnings-as-errors, complexity and TODO checks, the
  public-header audit, install to a staging prefix, and the downstream
  `find_package(scry)` consumer — via `scripts/ci-local.sh`.
- ASan+UBSan and TSan suites per commit (TSan keeps the
  `--repeat until-fail:3` flake detection).
- clang-format (pinned) and clang-tidy.
- The GCC 16 reflection leg: build, the then-27-test suite (current membership
  is governed by the amendment below), clean component install, downstream
  component consumer, compiled core-surface proof, and the separate
  ASan+UBSan rerun — via `scripts/ci-reflection.sh`.
- **Every existing behavioral test at acceptance.** The suites written to
  satisfy the retired gates are real tests; the gates went, not the tests.
  Their forward-maintenance lifetime is clarified below.
- Nightly CodeQL and long fuzz on all three protocol targets.

Retired or moved:

- **The coverage/CRAP gating stack is deleted** (`scripts/quality-gate.sh`,
  `quality_gate.py`, `quality_metrics.py`, `test_quality_gate.py`,
  `quality_coverage_mapper/`, the `quality` CI job). Coverage remains a
  reading habit (`llvm-cov report` when touching a pure component), not a CI
  actuary. QA-001/002/003/007 amended accordingly.
- **Mutation testing is deleted** (`mull.yml`, `ci-nightly-mutation.sh`, the
  nightly job).
- **The reflection coverage leg is deleted**
  (`scripts/reflection-coverage.sh` and its gcovr floors, including the
  decision-floor allowance for GCC's generated enum switch).
- **Per-PR short fuzzing folds into the nightly long-fuzz jobs**, removing
  the LLVM-21 apt toolchain from the PR ring. The deterministic golden,
  arbitrary-split, and boundary wire suites remain per-commit; the checked
  corpus continues to seed the nightly fuzz targets.
- **The showcase gate moves to the nightly ring** (`just showcase` locally).
  Its content is unchanged; it no longer gates PRs.
- **The Ollama local-model smoke becomes `workflow_dispatch`-only.** It
  exercises a live model, not the deterministic protocol seams that gate
  correctness.
- **The feasibility spikes are deleted** along with `SCRY_BUILD_CURL_SPIKE`
  and the `curl` preset. PORT-006's verification cell now points at the Curl
  transport and public-Harness integration tests.
- **`scripts/preflight.sh` mirrors the reduced PR ring exactly** (core,
  tidy, ASan+UBSan, TSan, reflection), preserving QA-011's one-command local
  equivalence. The `.agents/` preflight skill is deleted; AGENTS.md carries
  the workflow.

## Amendment: Reflection suite membership (2026-08-03)

The retained reflection gate is unchanged, but its live suite now contains 26
tests: 22 runtime/schema/codec/bridge/registration cases and four compile-fail
diagnostics. The annotation-only description contract retired two
trait-validation fixtures whose production surface was removed and added one
duplicate-annotation diagnostic. ADR 0007 records that clean-break contract.

The original 27-test count above remains as the acceptance-time snapshot for
this decision. This amendment governs the current `scripts/ci-reflection.sh`
membership; build, component installation, downstream consumption, compiled
core-surface isolation, and the separate ASan+UBSan rerun remain retained.

## Consequences

- `scripts/` drops from 14 files (~1,900 lines) to 6 (~510 lines); the PR
  ring drops from eleven runner legs across six pinned toolchains to eight
  legs across five (format, three matrix legs, tidy, two sanitizer legs,
  reflection), and the LLVM-21 apt toolchain leaves the PR ring entirely.
- A future change could reduce coverage on a pure component without a gate
  objecting. The accepted mitigations are review (a human now reads every
  diff), the retained suites (deleting tests is visible in review), and the
  QA-002 habit clause. If erosion is observed in practice, the cheap
  reintroduction is a single non-gating `llvm-cov report` line in CI output —
  not the retired apparatus.
- Fuzz regressions surface nightly rather than per-PR. The deterministic
  golden, arbitrary-split, and boundary wire suites still gate every commit,
  bounding the exposure to genuinely novel inputs.
- The showcase can break for at most a day before the nightly reports it;
  §8 classifies that feedback as immediate and cheap.
- If the project returns to unattended agent-driven development at scale,
  the retired machinery remains in git history (`git log --diff-filter=D`)
  and this ADR is the map for restoring it.

## Forward-maintenance clarification (2026-08-03)

The acceptance-time phrase "Every existing test" recorded what this decision
retained when it removed the metric machinery; it was not a promise to preserve
tests after their only production subject ceased to exist. Behavioral tests
remain while they exercise live behavior. A later reviewed change may delete a
test together with the implementation detail that was its sole subject, while
deleting a test merely because a retired metric once demanded it remains
outside this decision.
