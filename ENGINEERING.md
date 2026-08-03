# Scry — Engineering & Implementation Approach

Third of three: [DESIGN.md](DESIGN.md) says what we're building, [ARCHITECTURE.md](ARCHITECTURE.md) says how the code is shaped, this document says **how we work** — the quality machinery, gates, and habits that govern every commit. Philosophy over procedure; specific thresholds appear only where a gate needs a number to be enforceable.

---

## 1. Governing Philosophy

**Quality uses the strongest credible enforcement available.** Objective,
repeatable properties run automatically on every commit; change-specific
judgment that cannot be reduced to a trustworthy mechanical gate is handled by
explicit PR self-review. This is initially a solo project, so neither side may
be implicit: automated gates must fail loudly, and review obligations must be
written into the definition of done. Habits are named as habits rather than
misrepresented as machine-enforced standards.

**The architecture was designed for testability — the process must cash that check.** The sans-I/O state machine, injectable transport, and pure parsers exist so the hardest logic can be tested deterministically. If coverage on those components is low, that's not a testing failure, it's an architecture violation.

**Walking skeleton first.** The first thing that "works" is a thread of execution through every layer — public API → machine → adapter → transport → local server — doing the simplest useful thing (one streaming chat turn). Depth comes after the skeleton stands. This front-loads discovery of the integration risks (threading contract, curl lifetime, SSE realities) while the codebase is still cheap to reshape.

**Main is always green, always releasable.** Trunk-based development, short-lived branches, no long-running feature branches. Anything not ready to be on main hides behind a build flag (as the reflection layer already must).

**Gates are behavioral, not actuarial.** What gates a change is the physics:
the compiler matrix with warnings-as-errors, the deterministic test suites,
ASan/UBSan/TSan, clang-tidy, cyclomatic complexity, and the
install/package-consumer audits. Metric scoring — diff coverage, component
floors, CRAP — is not a gate; [ADR 0012](docs/adr/0012-release-infrastructure-simplification.md)
records why that apparatus was retired at the release posture and what would
justify restoring any of it.

## 2. Testing Plan

### Structure — the pyramid mirrors the architecture

| Layer | What | Character | Share of tests |
|---|---|---|---|
| Machine tests | Sans-I/O loop: event sequences in, command sequences asserted | Pure, deterministic, sub-ms, no threads | The bulk (~70%) |
| Component tests | SSE parser, retry classifier, reflected schema/codec, queue, pump budget | Pure or single-threaded; property-based where inputs are adversarial | Most of the rest |
| Adapter golden tests | Captured real wire payloads ↔ neutral model round-trips | Data-driven; payloads are checked-in fixtures | Thin |
| Integration tests | Real threads + fake transport; full harness against a local mock SSE server | The only tests where threading is real | Thin |
| Showcase contract tests | Deterministic NPC world and fake-controller panel behavior; real ImGui headless frame and package audit | Network-free, fixed state; the real dependency is compiled only in its opt-in leg | Thin |
| End-to-end smoke | Real local model (Ollama / llama.cpp server) | On demand, not per-commit; flakiness quarantined by design | Thinnest |

### Principles

- **Test behavior at seams, not implementation inside them.** Tests target the sanctioned interfaces (machine, adapter, transport). Refactoring internals must not break tests; if it does, the test was coupled to the wrong thing.
- **Fakes over mocks.** A hand-written fake transport with scriptable responses beats mock-framework expectations: fakes survive refactors and read as documentation. Mock frameworks are a smell here — the seams are few and narrow enough to fake properly.
- **Property-based testing where inputs are adversarial.** Both wire boundaries
  use exhaustive and fixed-seed random chunk splits plus a checked-in fuzz
  corpus. Reflection uses a deterministic compile-time family of
  supported/rejected struct shapes plus table-driven runtime JSON boundary
  cases; randomly generated reflection values remain future hardening.
- **Determinism is non-negotiable.** No real sleeps, no wall-clock time, no network in unit tests. Time is an injected event (the machine already "requests wake-ups"); a fake clock makes retry/backoff testable to the millisecond. A test that flakes gets fixed or deleted the day it flakes — a flaky suite trains you to ignore red, which destroys the entire system of gates.
- **Test-first for pure logic, test-with for plumbing.** The state machine, parsers, and classifiers are TDD-friendly (pure functions, crisp specs) — write tests first there. Threading and curl plumbing are exploratory — tests land in the same commit, shaped by what was learned.
- **Every bug becomes a test before it becomes a fix.** The reproduction (usually a machine-level event replay — this is why the sans-I/O design pays) is committed with the fix, permanently.

## 3. Coverage — A Habit, Not a Gate

Coverage is a **detector of untested code, not a target**. Chasing a percentage
produces assertion-free tests that execute code without checking it. Nothing in
CI scores it ([ADR 0012](docs/adr/0012-release-infrastructure-simplification.md));
the standing habits do the work:

- **New behavior ships with tests at the sanctioned seam** (§2); a bug fix
  ships with its reproduction.
- **The pure components stay near-totally covered.** The machine, parsers, and
  classifiers were designed for deterministic testing; untested branches there
  are architecture violations, found by reading `llvm-cov report` when touching
  them.
- **Coverage-off pragmas require a comment.** Silent exclusion is the metric's
  death.

## 4. Complexity & Size Limits

Enforced via lizard and clang-tidy on every commit:

- **Cyclomatic complexity:** warn at 10, fail at 15 per function. The known legitimate exception — the state machine's central `visit` dispatch — gets a named, documented suppression rather than a raised global limit; exceptions are enumerated, not diffuse.
- **Cognitive complexity** (clang-tidy `readability-function-cognitive-complexity`): fail at 25. Cyclomatic counts paths; cognitive counts nesting and interruption — both matter, they catch different sins.
- **Function length** (~60 lines) and **file length** (~500) are smells, not defects: lizard reports them, nothing gates them.
- **No `// TODO` without an issue link.** Unlinked TODOs are wishes; CI rejects any unlinked TODO outright.

## 5. Static & Dynamic Analysis

**Static — runs on every pull request:**

- clang-tidy with a curated, checked-in profile (bugprone-*, concurrency-*, cppcoreguidelines-* selectively, modernize-*, performance-*, readability-*). Curated means every disabled check has a one-line reason in the config — the config is documentation of our taste.
- Warnings-as-errors (`-Wall -Wextra -Wconversion -Wshadow`) on all compilers in the matrix; a warning that fires on only one compiler still fails.
- Include hygiene (IWYU) so the PImpl firewall from ARCHITECTURE.md remains real
  rather than aspirational. The reflection component adds an include-first
  standalone-header check and an installed consumer compiled without a Glaze
  include path; textual absence alone is not enough to prove the dependency
  firewall.
- CodeQL runs on the scheduled ring — deeper and slower than a PR gate should be.

**Dynamic — sanitizers are first-class build modes:**

- ASan + UBSan on the full unit/integration suite per pull request; TSan on all threaded tests per pull request. TSan especially is non-negotiable: the actor model's "no shared mutable state" claim is exactly the kind of invariant that erodes silently, and TSan is its enforcement mechanism. The sanitizer leg also configures `SCRY_ENABLE_LOGGING=ON`, so the opt-in diagnostic path is compiled and exercised rather than rotting behind a flag.
- **Fuzzing** (libFuzzer) covers the SSE, Anthropic, and OpenAI-compatible
  wire-JSON boundaries because they consume attacker-adjacent input (a
  compromised or buggy server must not crash the host app). The fuzz targets
  run with long budgets in the scheduled ring; the deterministic golden,
  arbitrary-split, and boundary wire tests remain per-commit.
- Valgrind/memcheck occasionally as a differently-shaped net; not gating.

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
2. **Scheduled weekly:** CodeQL, long fuzz runs on all three protocol targets,
   the showcase contract gate (a default-OFF leg that enables the examples,
   builds them with warnings as errors, runs the deterministic NPC and
   fake-panel cases, executes a real Dear ImGui headless frame, and repeats the
   package-absence audit), and the experimental reflection component gate — a
   fresh GCC 16/P2996-probed build, its schema/codec/bridge/registration and
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

**Everything CI does is orchestrated by one local command**
(`./scripts/preflight.sh`; `just ci` is an optional wrapper).
`just ci-fast` remains the optional wrapper for the quick core ring, and
`just showcase` runs the showcase gate locally. The full command runs every
leg, continues after failures, and identifies toolchains that the host cannot
provide; hosted CI is authoritative for those environments. A gate with no
local entry point is a gate you learn about only by pushing, which breeds
resentment and workarounds — even solo.

The public reference is generated by Doxygen 1.9.8 or newer with Graphviz
`dot`, through `scripts/ci-docs.sh`. Documentation warnings, missing public
symbol comments, undocumented enum values, missing parameter/return
documentation, and broken references fail the job, and the generated HTML is
retained as a CI artifact. Doxygen and Graphviz are build-only documentation
tools: neither participates in the normal CMake project, installed package,
exported targets, or consumer dependency graph. This separate entry point keeps
a docs toolchain failure from changing the C++23 library configuration surface
while remaining part of `preflight.sh` and the definition of done (QA-013).

## 7. Workflow & Change Hygiene

- **Trunk-based; PRs even solo.** The PR is the unit of self-review: a forced read of the diff, a written description, and green gates before merge. Squash-merge, conventional-commit messages (the changelog is generated, not written).
- **Decisions get ADRs.** Anything that would surprise a future contributor — or future us — gets a short Architecture Decision Record in `docs/adr/`. The evolution register in ARCHITECTURE.md §11 is the standing index of "deliberately simple" decisions; ADRs capture the one-off forks in the road.
- **Definition of Done** for any change: gates green; docs updated if behavior or a decision changed (the load-bearing docs — including [REQUIREMENTS.md](REQUIREMENTS.md), the normative register — are not ceremonial; a stale doc is a bug); public API changes come with a compiling example; deliberate simplifications added a row to the evolution register.
- **Dependency policy** (restating ARCHITECTURE.md §9 as process): new dependencies require a written justification committed with the change. Toolchains are pinned and upgraded deliberately — on this project the reflection compiler is experimental, so "toolchain drift" is a first-class risk tracked like a dependency.

## 8. Solo-Project Pragmatism

Where the line sits between rigor and overhead, decided in advance:

- **Rigor is non-negotiable where bugs are silent:** threading (TSan), the machine and parsers (deterministic suites + scheduled fuzz), API drift (golden files, package-consumer audits). These fail quietly in production and loudly in CI — that's the trade we're buying.
- **Pragmatism is fine where feedback is immediate:** example apps, demo polish, docs prose, CI plumbing itself. These fail visibly the moment they're wrong; gating them buys little.
- **Process weight is itself ratcheted — downward.** If a gate produces noise but never catches anything real for months, it gets demoted or deleted, with a note. The system of gates must stay credible, because the whole philosophy (§1) rests on actually trusting red to mean something. That clause has been exercised: [ADR 0011](docs/adr/0011-absolute-quality-gates.md) and [ADR 0012](docs/adr/0012-release-infrastructure-simplification.md) record what was demoted, deleted, or moved off the pull-request ring, and why.
