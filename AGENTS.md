# Scry repository guidance

## Project context

Scry is a C++ LLM harness for applications that own their main loop. The
stable surface is C++23; C++26 reflection is an explicitly isolated,
experimental capability.

Read the requirement tables in `docs/requirements.md` and the relevant sections
of the load-bearing design documents before changing behavior. Those state what
currently holds; do not implement or promise behavior they do not cover.

## Sources of truth

Use the current branch, not remembered project state.

1. `docs/requirements.md` is normative. Its RFC-2119 rows win if prose
   conflicts.
2. `docs/design/` defines product behavior and the public concepts.
3. `docs/architecture/` defines boundaries, dependency direction, and
   deliberate simplifications.
4. `docs/development/` defines quality gates and the definition of done.

Update the relevant document when a change alters behavior, architecture, a
requirement, a dependency decision, or a deliberate simplification. Reference
affected `SCRY-<AREA>-NNN` requirements in PR-facing summaries.

## Build and test

Run the core local gate from the repository root:

```sh
./scripts/ci-local.sh
```

This checks the diff, complexity, formatting, public-header boundaries,
the linked canonical example, contract tests, installation, and a downstream
`find_package(scry)` consumer. `just ci-fast` is an optional equivalent.

Run the complete local preflight before a PR handoff:

```sh
./scripts/preflight.sh
```

This adds clang-tidy, the ASan/UBSan and TSan suites, and the GCC 16
reflection leg. It runs every leg and reports unavailable host toolchains
explicitly; hosted CI remains authoritative for those legs. `just ci` is an
optional equivalent. Fuzzing, deep static analysis, the reflection gate, and
the showcase gate run in the scheduled weekly workflow
(`docs/development/quality-gates.md` §6); `just showcase` runs the showcase gate
locally.

For a normal edit/build loop:

```sh
cmake --preset dev
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

## Engineering guardrails

- Preserve the reflection-OFF C++23 build and keep reflection-only code behind
  `scry::reflection` and its build flags.
- Keep public headers self-contained and free of private implementation or
  third-party dependency leakage.
- Test behavior at sanctioned seams. Prefer deterministic fakes; do not use
  real sleeps, wall-clock time, or network access in unit tests.
- Treat warnings as errors. Keep functions within the checked complexity,
  length, and argument limits.
- Add a regression test before a bug fix and a compiling example for public API
  changes.
- Justify and pin every new dependency in the same change.
- Record deliberate shortcuts and their intended end state in the architecture
  evolution register.
- Do not edit generated files under `build/`, and do not commit build outputs.

## Definition of done

A change is done only when its relevant tests pass, the core local gate passes,
load-bearing documentation is current, and the final diff has been reviewed for
correctness, scope, and accidental artifacts. Report any check that could not
run and why.
