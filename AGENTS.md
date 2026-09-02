# Scry repository guidance

## Project context

Scry is a C++ LLM harness for applications that own their main loop. The public
API is C++26 and **requires GCC 16 or newer**: P2996 reflection is a core
feature, not an option, so there is no Clang consumer build. The implementation
under `src/` stays portable C++23 with no reflection syntax so clang-tidy and
libFuzzer can still compile it; `-DSCRY_CLANG_TOOLING=ON` selects exactly that
build and is not a supported consumer build.

Every preset pins `CMAKE_CXX_COMPILER` to `g++-16` through the hidden `gcc`
preset; override it on the command line when the local GCC 16 is spelled
differently.

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

Use the core gate as the quick inner loop while iterating:

```sh
./scripts/ci-local.sh
```

This checks the diff, complexity, formatting, public-header boundaries,
the linked canonical example, contract tests, installation, and a downstream
`find_package(scry)` consumer. `just ci-fast` is an optional equivalent.

Run the complete local preflight before every PR:

```sh
./scripts/preflight.sh
```

This adds the Doxygen site, the profiling smoke, clang-tidy, the ASan/UBSan and
TSan suites, and the fuzz corpus replay. It runs every leg, continues after
failures, reports a leg whose toolchain the host lacks as a skip rather than a
failure, and names every skipped leg again in the closing summary; hosted CI
remains authoritative for those. Each sanitizer leg probes its own flag with
`g++-16` first, because GCC ships no thread-sanitizer runtime on Apple Silicon.
`just ci` is an optional equivalent. On macOS, `brew install llvm@18` makes the
clang-tidy leg runnable locally — CI pins clang-tidy 18, so the keg-only
`llvm@18` is probed before the unversioned `llvm` formula. Long fuzz runs, deep
static analysis, and the showcase gate run in the scheduled weekly workflow
(`docs/development/quality-gates.md` §6); `just showcase` runs the showcase gate
locally.

For a normal edit/build loop:

```sh
cmake --preset dev
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

## Engineering guardrails

- Keep `src/**` free of reflection syntax so the `SCRY_CLANG_TOOLING` build
  (clang-tidy, libFuzzer) keeps compiling; reflection lives in the public
  headers.
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
