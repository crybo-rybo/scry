# ADR 0004: Live Quality Ratchet

- Status: Accepted
- Date: 2026-07-17

## Context

QA-001, QA-003, and QA-007 require diff branch coverage, CRAP gating, and
quality metrics that cannot regress from `main`. A checked-in numeric baseline
would be vulnerable to stale updates and to coverage-map drift between local
AppleClang and the pinned Linux Clang CI toolchain.

## Decision

- Build the merge-base and the candidate tree with the same Clang compiler in
  one quality-gate invocation.
- Collect branch data from `llvm-cov` and complexity data from the already
  pinned lizard 1.22.1 tool.
- Require at least 90% branch-aware coverage on changed production lines. A
  changed decision counts both branch outcomes; changed executable lines
  without a decision count once.
- Reject CRAP scores above 30 and print the ten highest scores on every run.
- Ratchet total branch coverage and the counts of complexity warnings, long
  functions, long files, CRAP violations, and unlinked TODOs.
- Keep absolute complexity, CRAP, warnings, and sanitizer gates authoritative.
  Raw complexity below its warning threshold is reported but not ratcheted;
  freezing M0's small maximum would prevent reasonable M1 functions.
- Require coverage exclusions to carry an inline
  `SCRY-COVERAGE-JUSTIFICATION:` explanation.

The analyzer uses only Python's standard library and compiler-provided LLVM
tools. It adds no runtime or consumer dependency.

## Consequences

The comparison is slower than checking a committed snapshot because it builds
the test suite twice. In return, both sides use the same tools, the baseline
cannot be hand-edited, and local results match the comparison performed in CI.
The generated report remains under ignored `build/quality/` for inspection.
