# Scry — Engineering & Implementation Approach

Third of three: [DESIGN.md](DESIGN.md) says what we're building, [ARCHITECTURE.md](ARCHITECTURE.md) says how the code is shaped, this document says **how we work** — the quality machinery, gates, and habits that govern every commit. Philosophy over procedure; specific thresholds appear only where a gate needs a number to be enforceable.

---

## 1. Governing Philosophy

**Quality is enforced by machines, not remembered by humans.** This is (initially) a solo project, which means no reviewer will catch what CI doesn't. Every standard in this document either runs automatically on every commit or it does not exist. Aspirational rules that depend on discipline decay; gates don't.

**The architecture was designed for testability — the process must cash that check.** The sans-I/O state machine, injectable transport, and pure parsers exist so the hardest logic can be tested deterministically. If coverage on those components is low, that's not a testing failure, it's an architecture violation.

**Walking skeleton first.** The first milestone that "works" is a thread of execution through every layer — public API → machine → adapter → transport → local server — doing the simplest useful thing (one streaming chat turn). Depth comes after the skeleton stands. This front-loads discovery of the integration risks (threading contract, curl lifetime, SSE realities) while the codebase is still cheap to reshape.

**Main is always green, always releasable.** Trunk-based development, short-lived branches, no long-running feature branches. Anything not ready to be on main hides behind a build flag (as the reflection layer already must).

**Ratchet, never regress.** Every quality metric — coverage, complexity debt,
warnings, sanitizer cleanliness — may improve or hold, never decline. Gates
compare the candidate against its merge-base on main with the same toolchain,
not against an editable snapshot. Numeric debt counts ratchet while absolute
limits remain authoritative; raw complexity below the warning threshold is
reported rather than frozen at M0's unusually small maximum.

## 2. Testing Plan

### Structure — the pyramid mirrors the architecture

| Layer | What | Character | Share of tests |
|---|---|---|---|
| Machine tests | Sans-I/O loop: event sequences in, command sequences asserted | Pure, deterministic, sub-ms, no threads | The bulk (~70%) |
| Component tests | SSE parser, retry classifier, reflected schema/codec, queue, pump budget | Pure or single-threaded; property-based where inputs are adversarial | Most of the rest |
| Adapter golden tests | Captured real wire payloads ↔ neutral model round-trips | Data-driven; payloads are checked-in fixtures | Thin |
| Integration tests | Real threads + fake transport; full harness against a local mock SSE server | The only tests where threading is real | Thin |
| End-to-end smoke | Real local model (Ollama / llama.cpp server) in CI | Nightly, not per-commit; flakiness quarantined by design | Thinnest |

### Principles

- **Test behavior at seams, not implementation inside them.** Tests target the sanctioned interfaces (machine, adapter, transport). Refactoring internals must not break tests; if it does, the test was coupled to the wrong thing.
- **Fakes over mocks.** A hand-written fake transport with scriptable responses beats mock-framework expectations: fakes survive refactors and read as documentation. Mock frameworks are a smell here — the seams are few and narrow enough to fake properly.
- **Property-based testing where inputs are adversarial.** The SSE parser uses
  exhaustive and fixed-seed random chunk splits. M3 reflection uses a
  deterministic compile-time family of supported/rejected struct shapes plus
  table-driven runtime JSON boundary cases. Randomly generated reflection
  values remain future hardening and are not claimed by the live M3 gate.
- **Determinism is non-negotiable.** No real sleeps, no wall-clock time, no network in unit tests. Time is an injected event (the machine already "requests wake-ups"); a fake clock makes retry/backoff testable to the millisecond. A test that flakes gets fixed or deleted the day it flakes — a flaky suite trains you to ignore red, which destroys the entire system of gates.
- **Test-first for pure logic, test-with for plumbing.** The state machine, parsers, and classifiers are TDD-friendly (pure functions, crisp specs) — write tests first there. Threading and curl plumbing are exploratory — tests land in the same commit, shaped by what was learned.
- **Every bug becomes a test before it becomes a fix.** The reproduction (usually a machine-level event replay — this is why the sans-I/O design pays) is committed with the fix, permanently.

## 3. Coverage — Measured Honestly, Gated Intelligently

Coverage is a **detector of untested code, not a target**. Chasing a global percentage produces assertion-free tests that execute code without checking it. The gates are therefore structural:

- **Instrumentation:** llvm-cov / gcov, *branch* coverage not just line — branch coverage is what catches the untested error path, and error paths are half this library.
- **Diff coverage gate (the primary gate):** new/changed lines in a PR must meet a high branch-coverage bar (~90%). This is stricter than a global gate where it matters (the code being written now) and doesn't punish present work for past sins.
- **Per-component floors, not one global number:** the sans-I/O machine, parsers, and classifiers are pure — they carry a near-total floor (95%+ branch). Transport/curl plumbing carries a lower floor with the gap covered by integration tests and sanitizers. One global number would let untested machine logic hide behind well-covered plumbing.
- **Exclusions are visible and justified:** coverage-off pragmas require a comment and appear in review diffs. Silent exclusion is the metric's death.
- **Collection is deterministic:** reliability repeats remain ordinary CTest
  runs, while coverage repeats use fixed Catch2 ordering, profile names, and
  atomic profile-counter updates for the worker/app-thread runtime. `llvm-cov`
  exports against one whole-archive mapper built from the installed package,
  avoiding lost concurrent counter updates and unstable or mismatched mappings
  from a many-executable export without changing which production branches are
  counted.

**Consteval coverage is reported honestly.** Runtime instrumentation cannot
observe a branch executed only by the compiler while forming
`input_schema_v<Args>`. The live M3 gate covers type-directed branches with
compile-time schema goldens, concept assertions, and compile-fail fixtures;
they are not assigned a fabricated runtime percentage. Instrumentable M3 code
has a separate pinned GCC 16/gcovr 8.6 gate. Adjusted source decisions in
`reflection_codec.hpp` must reach 95%, every codec function must execute, and
GCC/gcovr CFG branches in compiled `json_bridge.cpp` must reach 95%. The
current gated results are 32/32 codec decisions, 62/62 codec functions, and
97/97 bridge branches.

The adjustment excludes exactly one inline-marked GCC-generated switch on the
reflected-enum decoder's definition line. The checked validator fails unless
there is exactly one excluded switch and rejects malformed or widened metrics.
The gate still prints the unadjusted codec decisions (33/37, 89.2%) and combined
GCC/gcovr CFG arcs after standard exception/unreachable/non-code exclusions and
line merging (405/462, 87.7%). Those diagnostics include duplicated template,
destructor, and exception-flow arcs and are not presented as the normative
source-decision metric.

### CRAP gating — where coverage meets complexity

CRAP (Change Risk Anti-Patterns) scores each function: `CRAP(f) = cc(f)² × (1 − cov(f))³ + cc(f)`, where `cc` is cyclomatic complexity and `cov` is branch coverage. It formalizes the real risk rule: **complex and untested is unacceptable; simple and untested is tolerable; complex and tested is watched.**

- Hard gate: no function on main with CRAP > 30.
- Watch list: CI reports the top-10 CRAP scores every run, so creeping risk is visible before it gates.
- The two exits from a CRAP violation — test it or simplify it — are both wins. That's why this is the flagship gate: it never incentivizes a bad response.

## 4. Complexity & Size Limits

Enforced via lizard and clang-tidy on every commit:

- **Cyclomatic complexity:** warn at 10, fail at 15 per function. The known legitimate exception — the state machine's central `visit` dispatch — gets a named, documented suppression rather than a raised global limit; exceptions are enumerated, not diffuse.
- **Cognitive complexity** (clang-tidy `readability-function-cognitive-complexity`): fail at 25. Cyclomatic counts paths; cognitive counts nesting and interruption — both matter, they catch different sins.
- **Function length** warn at ~60 lines; **file length** warn at ~500. Warnings, not gates — length is a smell, not a defect — but warnings ratchet (§1): the counts may not grow.
- **No `// TODO` without an issue link.** Unlinked TODOs are wishes; CI counts them and the count may not rise.

## 5. Static & Dynamic Analysis

**Static — runs on every commit:**

- clang-tidy with a curated, checked-in profile (bugprone-*, concurrency-*, cppcoreguidelines-* selectively, modernize-*, performance-*, readability-*). Curated means every disabled check has a one-line reason in the config — the config is documentation of our taste.
- Warnings-as-errors (`-Wall -Wextra -Wconversion -Wshadow`) on all compilers in the matrix; a warning that fires on only one compiler still fails.
- Include hygiene (IWYU) so the PImpl firewall from ARCHITECTURE.md remains real
  rather than aspirational. The reflection component adds an include-first
  standalone-header check and an installed consumer compiled without a Glaze
  include path; textual absence alone is not enough to prove the dependency
  firewall.
- clang static analyzer / CodeQL on a schedule (deeper, slower analyses run nightly, not per-commit).

**Dynamic — sanitizers are first-class build modes from M0:**

- ASan + UBSan on the full unit/integration suite per commit; TSan on all threaded tests per commit. TSan especially is non-negotiable: the actor model's "no shared mutable state" claim is exactly the kind of invariant that erodes silently, and TSan is its enforcement mechanism. The supported GCC reflection leg additionally runs its marshalling and Scry-owned JSON bridge under ASan+UBSan.
- **Fuzzing** (libFuzzer) covers the SSE and Anthropic wire-JSON boundaries
  because they consume attacker-adjacent input (a compromised or buggy server
  must not crash the host app). Reflection decoding has deterministic boundary
  tests but no claimed property/fuzz gate in M3. Short live fuzz runs execute
  in CI; long runs remain nightly work.
- Valgrind/memcheck occasionally as a differently-shaped net; not gating.

## 6. CI Pipeline Shape

Three rings, ordered by feedback speed; a failure in an inner ring stops the outer ones:

1. **Per-commit (< ~10 min):** format check, tidy, build matrix (supported
   GCC 16 component with reflection ON; stable GCC/Clang with reflection OFF —
   the severability proof), unit + component tests, deterministic
   fake-transport and local-loopback integration tests, adapter golden suites,
   ASan/UBSan/TSan suites, short fuzz, diff coverage, CRAP, and complexity
   gates. The M3 reflection leg also installs to a clean prefix and builds/runs
   a downstream `find_package(scry CONFIG REQUIRED COMPONENTS reflection)`
   consumer. clang-p2996 is manual, non-gating compatibility work and never
   builds installable or release artifacts.
2. **Per-merge to main:** publish retained integration and coverage reports and perform release-oriented packaging checks.
3. **Nightly:** end-to-end against a real local model, long fuzz, deep static analysis, mutation testing (mutate the machine and parsers; surviving mutants reveal assertion-free tests — this audits the *tests*, which coverage cannot).

**Everything CI does is orchestrated by one local command**
(`./scripts/preflight.sh`; `just ci` is an optional wrapper).
`just ci-fast` remains the optional wrapper for the quick core ring. The full
command runs every leg, continues after failures, and identifies toolchains
that the host cannot provide; hosted CI is authoritative for those
environments. A gate with no local entry point is a gate you learn about only
by pushing, which breeds resentment and workarounds — even solo.

The shared `scripts/ci-reflection.sh` gate is called by local preflight and
hosted CI. It performs a fresh GCC 16/P2996 build, runs the full 265-test suite
with `--repeat until-fail:3` (27 reflection-labelled tests: 22
runtime/schema/codec/bridge cases plus five compile-fail diagnostics), audits
and consumes a clean reflection install, then reruns the reflection tests in a
separate ASan+UBSan build and calls `scripts/reflection-coverage.sh`; all 27
reflection-labelled tests pass in both modes. The core gate separately audits a
clean
reflection-OFF install and downstream consumer. M3 evidence does not claim a
manual clang-p2996 run, randomized reflection property generation, or a
reflection fuzz target.

## 7. Workflow & Change Hygiene

- **Trunk-based; PRs even solo.** The PR is the unit of self-review: a forced read of the diff, a written description, and green gates before merge. Squash-merge, conventional-commit messages (the changelog is generated, not written).
- **Decisions get ADRs.** Anything that would surprise a future contributor — or future us — gets a short Architecture Decision Record in `docs/adr/`. The evolution register in ARCHITECTURE.md §11 is the standing index of "deliberately simple" decisions; ADRs capture the one-off forks in the road.
- **Definition of Done** for any change: gates green; docs updated if behavior or a decision changed (the load-bearing docs — including [REQUIREMENTS.md](REQUIREMENTS.md), the normative register — are not ceremonial; a stale doc is a bug); requirement rows updated to name their verifying test once it exists; public API changes come with a compiling example; deliberate simplifications added a row to the evolution register.
- **Dependency policy** (restating ARCHITECTURE.md §9 as process): new dependencies require a written justification committed with the change. Toolchains are pinned and upgraded deliberately — on this project the compilers are experimental, so "toolchain drift" is a first-class risk tracked like a dependency.

## 8. Solo-Project Pragmatism

Where the line sits between rigor and overhead, decided in advance:

- **Rigor is non-negotiable where bugs are silent:** threading (TSan), the machine (coverage floor + CRAP), parsers (fuzz + property tests), API drift (golden files). These fail quietly in production and loudly in CI — that's the trade we're buying.
- **Pragmatism is fine where feedback is immediate:** example apps, demo polish, docs prose, CI plumbing itself. These fail visibly the moment they're wrong; gating them buys little.
- **Process weight is itself ratcheted — downward.** If a gate produces noise but never catches anything real for months, it gets demoted or deleted, with a note. The system of gates must stay credible, because the whole philosophy (§1) rests on actually trusting red to mean something.
