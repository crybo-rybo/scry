---
name: scry-preflight
description: Validate Scry changes before a pull-request handoff or readiness decision. Use when Codex is asked to preflight, validate, prepare, publish, or review a Scry change; confirm its required local gates; inspect CI status; or report whether work is ready to merge. Do not use for an ordinary edit-build loop that only needs the directly affected test.
---

# Scry Preflight

Perform the repository's complete, evidence-backed validation workflow. Keep
the report explicit about checks that ran, checks delegated to hosted CI, and
remaining risk.

## 1. Establish scope

From the repository root:

```sh
git status --short --branch
git diff --stat
git diff --check
```

Include staged changes or the branch range when that is the requested review
scope. Read `AGENTS.md`, then map behavior or API changes to the relevant
`REQUIREMENTS.md` rows and load-bearing documents.

Stop and report before overwriting unrelated user changes. Ignore generated
contents under `build/`.

## 2. Run the core gate

Run:

```sh
./scripts/ci-local.sh
```

Do not replace this with an improvised subset. It is the local source of truth
for formatting, complexity, build, tests, installation, and package-consumer
validation.

## 3. Select extended gates

Run extended gates in proportion to the changed surface:

- C++ headers, sources, examples, tests, or CMake: clang-tidy and the relevant
  sanitizer presets.
- Threading, ownership, callbacks, or concurrency: TSan is required.
- Memory, lifetime, parsing, or error-path behavior: ASan + UBSan is required.
- libcurl, transport, or SSE changes: the curl feasibility preset is required.
- Reflection, schema generation, or reflection build logic: the GCC 16
  reflection target is required, normally through hosted CI if that toolchain
  is unavailable locally.
- Documentation-only changes: the core gate is sufficient unless the docs
  change commands, packaging, public contracts, or build behavior.

Use these commands:

```sh
cmake --preset ci -B build/tidy \
  -DSCRY_ENABLE_CLANG_TIDY=ON \
  -DSCRY_ENABLE_FORMAT_CHECK=OFF
cmake --build build/tidy

cmake --preset asan
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure

cmake --preset tsan
cmake --build build/tsan
ctest --test-dir build/tsan --output-on-failure

cmake --preset curl
cmake --build build/curl
ctest --test-dir build/curl --output-on-failure
```

On macOS, if Homebrew LLVM supplies clang-tidy but is not on `PATH`, prepend
`"$(brew --prefix llvm)/bin"` for the tidy configure and build. Do not install
or upgrade a toolchain merely to finish a review without stating that scope
change.

## 4. Review the result

Inspect the final diff and check:

- normative requirements and implementation agree;
- public API changes have a compile-checked example;
- tests assert behavior at the correct seam;
- reflection and third-party dependencies remain isolated;
- no runtime behavior from a later milestone slipped into the change;
- no generated output, debug code, unlinked TODO, or unrelated cleanup remains;
- dependency and architecture decisions are documented.

If a PR exists and GitHub access is available, inspect its current checks.
Treat the hosted compiler matrix and GCC 16 reflection job as authoritative for
toolchains that are not available locally.

## 5. Report readiness

Lead with one verdict: ready, ready with noted hosted checks, or not ready.
List every executed gate with its result, then list skipped or unavailable
gates with a reason. End with concrete remaining work and residual risk. Never
claim a check passed based only on its presence in CI configuration.
