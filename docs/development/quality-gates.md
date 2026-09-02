# Development: Quality Gates

## 4. Complexity & Size Limits

Enforced via lizard and clang-tidy on every commit:

- **Cyclomatic complexity:** warn at 10, fail at 15 per function. The known legitimate exception — the state machine's central `visit` dispatch — gets a named, documented suppression rather than a raised global limit; exceptions are enumerated, not diffuse.
- **Cognitive complexity** (clang-tidy `readability-function-cognitive-complexity`): fail at 25. Cyclomatic counts paths; cognitive counts nesting and interruption — both matter, they catch different sins.
- **Function length** (~60 lines) and **file length** (~500) are smells, not defects: lizard reports them, nothing gates them.
- **No `// TODO` without an issue link.** Unlinked TODOs are wishes; CI rejects any unlinked TODO outright.

## 5. Static & Dynamic Analysis

**Static — runs on every pull request:**

- clang-tidy with a curated, checked-in profile (bugprone-*, concurrency-*, performance-*, a selective modernize-* pair, and readability-function-cognitive-complexity plus readability-redundant-*). Curated means every disabled check has a one-line reason in the config — the config is documentation of the project's taste.
- Warnings-as-errors (`-Wall -Wextra -Wconversion -Wshadow`) on all compilers in the matrix; a warning that fires on only one compiler still fails.
- Include hygiene (IWYU) so the PImpl firewall from the
  [architecture overview](../architecture/overview.md) remains real
  rather than aspirational. The reflection component adds an include-first
  standalone-header check and an installed consumer compiled without a Glaze
  include path; textual absence alone is not enough to prove the dependency
  firewall.
- CodeQL runs on the scheduled ring — deeper and slower than a PR gate should be.

**Dynamic — sanitizers are first-class build modes:**

- ASan + non-recovering UBSan on the full unit/integration suite per pull request; TSan on all threaded tests per pull request. TSan especially is non-negotiable: the actor model's "no shared mutable state" claim is exactly the kind of invariant that erodes silently, and TSan is its enforcement mechanism. The sanitizer leg also configures `SCRY_ENABLE_LOGGING=ON`, so the opt-in diagnostic path is compiled and exercised rather than rotting behind a flag.
- **Fuzzing** (libFuzzer) covers the SSE, Anthropic, and OpenAI-compatible
  wire-JSON boundaries under ASan + non-recovering UBSan because they consume attacker-adjacent input (a
  compromised or buggy server must not crash the host app). The fuzz targets
  run with long budgets in the scheduled ring; the deterministic golden,
  arbitrary-split, and boundary wire tests remain per-commit.
- Valgrind/memcheck occasionally as a differently-shaped net; not gating.

### Performance evidence — informational, not a timing gate

The opt-in profiling suite is separate from sanitizer and correctness builds.
It may be built and smoke-run in hosted CI, but shared-runner timings, C++
allocation counts, and RSS values do not pass or fail a pull request. A
performance or memory claim instead supplies same-host paired evidence under
`SCRY-QA-014`, following the
[performance profiling protocol](performance-profiling.md). This preserves
`SCRY-QA-007`: the compiler, tests, sanitizers, static analysis, complexity, and
package audits remain the gates, while measurements inform whether an
optimization deserves to exist.

`SCRY_BUILD_BENCHMARKS` defaults to `OFF`. Google Benchmark and the profiling
executables are build-only; they never enter the installed or exported package.
A profiling workflow checks that the harness configures, builds, validates all
semantic oracles, emits parseable artifacts, and exercises a one-scenario,
one-cycle paired orchestration smoke. That paired result is explicitly
diagnostic and evidence-ineligible. The workflow does not enforce an absolute
timing percentage. The evolution register defines the evidence needed before
any scenario-specific threshold could become credible.

## 6. CI Pipeline Shape

Three rings, ordered by feedback speed; a failure in an inner ring stops the outer ones:

1. **Pull request (fast):** one merged hygiene job (warning-clean Doxygen API
   site plus the pinned format check), the core matrix — Linux GCC 14, Linux
   Clang 18 with libc++, AppleClang on macOS 15 — running unit, component,
   golden, deterministic fake-transport and local-loopback integration suites
   with warnings-as-errors and the complexity gates, clang-tidy, an ASan+UBSan
   leg (which also compiles `SCRY_ENABLE_LOGGING=ON`), and a TSan leg. The core
   matrix installs to a clean prefix and builds a downstream
   `find_package(scry)` consumer, proving the reflection-OFF package surface.
   A path-aware reflection gate additionally runs the full GCC 16 reflection
   leg — including its ASan+UBSan rerun, the component's only sanitizer
   coverage — on any pull request touching a reflection-affecting path, so
   component regressions cannot merge untested while unrelated pull requests
   skip the experimental toolchain. A separate path-aware profiling job builds
   both opt-in executables, runs every semantic oracle in smoke mode, and
   validates the artifact contract without applying a timing threshold.
2. **Scheduled weekly:** CodeQL, long fuzz runs on all three protocol targets,
   the showcase contract gate (a default-OFF leg that enables the examples,
   builds them with warnings as errors, runs the deterministic NPC and
   fake-panel cases (including the NPC reasoning-control request), executes a
   real Dear ImGui headless frame, and repeats the
   package-absence audit), and the experimental reflection component gate — a
   fresh GCC 16/P2996/P3394 capability-probed build, its
   schema/codec/bridge/registration and
   compile-fail suites, a clean component install, a downstream
   `find_package(scry CONFIG REQUIRED COMPONENTS reflection)` consumer, a
   core-only C++23 consumer compiled against the same reflection-enabled
   install, and a separate ASan+UBSan rerun.
3. **On demand (`workflow_dispatch`):** a bounded end-to-end smoke against a
   checksum-pinned local OpenAI-compatible server. The pinned versions and
   model tag live in the workflow file. The smoke uses a health check, hard
   startup/turn/job timeouts, one chat case and one tool round, and retained
   diagnostics on failure. It exercises a live model, not the deterministic
   protocol seams, so it never enters a gating ring.

**Everything in the per-commit ring is orchestrated by one local command**
(`./scripts/preflight.sh`; `just ci` is an optional wrapper).
`just ci-fast` remains the optional wrapper for the quick core ring, and
`just showcase` runs the showcase gate locally. The full command runs every
ring leg, continues after failures, and identifies toolchains that the host
cannot provide; hosted CI is authoritative for those environments. The
scheduled-ring legs (CodeQL, long fuzz, showcase) and the manual local-model
smoke are not part of the one-command ring. A gate with no local entry point is
a gate you learn about only by pushing, which breeds workarounds.

The public reference is generated by Doxygen 1.9.8 or newer with Graphviz
`dot`, through `scripts/ci-docs.sh`. Documentation warnings, missing public
symbol comments, undocumented enum values, missing parameter/return
documentation, and broken references fail the job, and the generated HTML is
retained as a CI artifact. Doxygen and Graphviz are build-only documentation
tools: neither participates in the normal CMake project, installed package,
exported targets, or consumer dependency graph. This separate entry point keeps
a docs toolchain failure from changing the C++23 library configuration surface
while remaining part of `preflight.sh` and the definition of done (QA-013).
