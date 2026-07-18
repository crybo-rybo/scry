# Scry repository guidance

## Project context

Scry is a C++ LLM harness for applications that own their main loop. The
stable surface is C++23; C++26 reflection is an explicitly isolated,
experimental capability.

The repository advances by milestones. Read the status in `README.md` and the
milestone column in `REQUIREMENTS.md` before changing behavior. Do not
silently implement or promise behavior assigned to a later milestone.

## Sources of truth

Use the current branch, not remembered project state.

1. `REQUIREMENTS.md` is normative. Its RFC-2119 rows win if prose conflicts.
2. `DESIGN.md` defines product behavior and the public concepts.
3. `ARCHITECTURE.md` defines boundaries, dependency direction, and deliberate
   simplifications.
4. `ENGINEERING.md` defines quality gates and the definition of done.
5. Accepted decisions live in `docs/adr/`.

Update the relevant document when a change alters behavior, architecture, a
requirement, a dependency decision, or a deliberate simplification. Reference
affected `SCRY-<AREA>-NNN` requirements in PR-facing summaries.

## Build and test

Run the core local gate from the repository root:

```sh
./scripts/ci-local.sh
```

This checks the diff, complexity, formatting, public-header boundaries,
compile-only examples, contract tests, installation, and a downstream
`find_package(scry)` consumer. `just ci-fast` is an optional equivalent.

Run the complete local preflight before a PR handoff:

```sh
just ci
```

This adds the branch-coverage/CRAP ratchet, clang-tidy, sanitizers, curl, and
reflection feasibility. It runs every leg and reports unavailable host
toolchains explicitly; hosted CI remains authoritative for those legs.

For a normal edit/build loop:

```sh
cmake --preset dev
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

Use `$scry-preflight` before a PR handoff or when asked for release/readiness
validation. It owns the extended clang-tidy, sanitizer, feasibility, and diff
review workflow.

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
load-bearing documentation is current, and the final diff has been reviewed
for correctness, scope, and accidental artifacts. Report any check that could
not run and why.
