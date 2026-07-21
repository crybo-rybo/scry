# Scry — Engineering & Implementation Approach

Third of three: [DESIGN.md](DESIGN.md) says what we're building, [ARCHITECTURE.md](ARCHITECTURE.md) says how the code is shaped, this document says **how we work** — the quality machinery, gates, and habits that govern every commit. Philosophy over procedure; specific thresholds appear only where a gate needs a number to be enforceable.

---

## 1. Governing Philosophy

**Quality is enforced by machines, not remembered by humans.** This is (initially) a solo project, which means no reviewer will catch what CI doesn't. Every standard in this document either runs automatically on every commit or it does not exist. Aspirational rules that depend on discipline decay; gates don't.

**The architecture was designed for testability — the process must cash that check.** The sans-I/O state machine, injectable transport, and pure parsers exist so the hardest logic can be tested deterministically. If coverage on those components is low, that's not a testing failure, it's an architecture violation.

**Walking skeleton first.** The first milestone that "works" is a thread of execution through every layer — public API → machine → adapter → transport → local server — doing the simplest useful thing (one streaming chat turn). Depth comes after the skeleton stands. This front-loads discovery of the integration risks (threading contract, curl lifetime, SSE realities) while the codebase is still cheap to reshape.

**Main is always green, always releasable.** Trunk-based development, short-lived branches, no long-running feature branches. Anything not ready to be on main hides behind a build flag (as the reflection layer already must).

**Gates are behavioral, not actuarial.** What gates every commit is the
physics: the compiler matrix with warnings-as-errors, the deterministic test
suites, ASan/UBSan/TSan, clang-tidy, cyclomatic complexity, and the
install/package-consumer audits. The earlier metrics apparatus — diff branch
coverage, per-component coverage floors, an aggregate backstop, and CRAP
scoring, enforced by a bespoke ~1,100-line analyzer with its own unit tests —
was retired at the v0.0.1 release posture under §8 and
[ADR 0012](docs/adr/0012-release-infrastructure-simplification.md). It was
built so unattended agent-driven development could prove its work without a
human reviewer; with a human maintainer reading every diff, red/green on real
tests carries the same information at a fraction of the maintenance surface.
(The merge-base ratchet that preceded it was demoted earlier, under
[ADR 0011](docs/adr/0011-absolute-quality-gates.md).) The test suites written
under those gates remain in full — the gates went, not the tests.

## 2. Testing Plan

### Structure — the pyramid mirrors the architecture

| Layer | What | Character | Share of tests |
|---|---|---|---|
| Machine tests | Sans-I/O loop: event sequences in, command sequences asserted | Pure, deterministic, sub-ms, no threads | The bulk (~70%) |
| Component tests | SSE parser, retry classifier, reflected schema/codec, queue, pump budget | Pure or single-threaded; property-based where inputs are adversarial | Most of the rest |
| Adapter golden tests | Captured real wire payloads ↔ neutral model round-trips | Data-driven; payloads are checked-in fixtures | Thin |
| Integration tests | Real threads + fake transport; full harness against a local mock SSE server | The only tests where threading is real | Thin |
| Showcase contract tests | Deterministic NPC world and fake-controller panel behavior; real ImGui headless frame and package audit | Network-free, fixed state; the real dependency is compiled only in its opt-in leg | Thin |
| End-to-end smoke | Real local model (Ollama / llama.cpp server) in CI | On demand, not per-commit; flakiness quarantined by design | Thinnest |

### Principles

- **Test behavior at seams, not implementation inside them.** Tests target the sanctioned interfaces (machine, adapter, transport). Refactoring internals must not break tests; if it does, the test was coupled to the wrong thing.
- **Fakes over mocks.** A hand-written fake transport with scriptable responses beats mock-framework expectations: fakes survive refactors and read as documentation. Mock frameworks are a smell here — the seams are few and narrow enough to fake properly.
- **Property-based testing where inputs are adversarial.** The SSE parser uses
  exhaustive and fixed-seed random chunk splits. The M4 OpenAI-compatible wire
  boundary applies the same arbitrary-split discipline and adds a checked fuzz
  corpus. M3
  reflection uses a deterministic compile-time family of supported/rejected
  struct shapes plus table-driven runtime JSON boundary cases. Randomly
  generated reflection values remain future hardening and are not claimed by
  the live M3 gate.
- **Determinism is non-negotiable.** No real sleeps, no wall-clock time, no network in unit tests. Time is an injected event (the machine already "requests wake-ups"); a fake clock makes retry/backoff testable to the millisecond. A test that flakes gets fixed or deleted the day it flakes — a flaky suite trains you to ignore red, which destroys the entire system of gates.
- **Test-first for pure logic, test-with for plumbing.** The state machine, parsers, and classifiers are TDD-friendly (pure functions, crisp specs) — write tests first there. Threading and curl plumbing are exploratory — tests land in the same commit, shaped by what was learned.
- **Every bug becomes a test before it becomes a fix.** The reproduction (usually a machine-level event replay — this is why the sans-I/O design pays) is committed with the fix, permanently.

## 3. Coverage — A Habit, Not a Gate

Coverage is a **detector of untested code, not a target**. Chasing a
percentage produces assertion-free tests that execute code without checking
it. During milestone development the project enforced coverage mechanically:
diff branch coverage ≥ 90%, ≥ 95% branch floors on the sans-I/O machine, SSE
parser, and retry classifier, an 88% aggregate backstop, per-function CRAP ≤
30, and a pinned gcovr gate on the reflection codec/bridge — all measured by
a bespoke analyzer under `scripts/`. Those gates did their job: the suites
they demanded exist, the last full run measured 93.3% diff branch coverage
and a maximum CRAP of 13.1, and every error path in the machine, parsers,
and classifiers has a test asserting it.

At the v0.0.1 release posture
([ADR 0012](docs/adr/0012-release-infrastructure-simplification.md)) the
enforcement machinery was deleted, not the discipline. The standing habits:

- **New behavior ships with tests at the sanctioned seam** (§2); a bug fix
  ships with its reproduction. This is what the diff-coverage gate was
  approximating.
- **The pure components stay near-totally covered.** The machine, parsers,
  and classifiers were designed for deterministic testing; untested branches
  there are architecture violations, found by reading `llvm-cov report`
  when touching them, not by a CI actuary.
- **Coverage-off pragmas require a comment.** Silent exclusion is the
  metric's death.

**Consteval coverage is reported honestly.** Runtime instrumentation cannot
observe a branch executed only by the compiler while forming
`input_schema_v<Args>`. Type-directed branches are covered by compile-time
schema goldens, concept assertions, and compile-fail fixtures; they are never
assigned a fabricated runtime percentage.

## 4. Complexity & Size Limits

Enforced via lizard and clang-tidy on every commit:

- **Cyclomatic complexity:** warn at 10, fail at 15 per function. The known legitimate exception — the state machine's central `visit` dispatch — gets a named, documented suppression rather than a raised global limit; exceptions are enumerated, not diffuse.
- **Cognitive complexity** (clang-tidy `readability-function-cognitive-complexity`): fail at 25. Cyclomatic counts paths; cognitive counts nesting and interruption — both matter, they catch different sins.
- **Function length** (~60 lines) and **file length** (~500) are smells, not defects: lizard reports them, nothing gates them.
- **No `// TODO` without an issue link.** Unlinked TODOs are wishes; CI rejects any unlinked TODO outright.

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
- **Fuzzing** (libFuzzer) covers the SSE, Anthropic, and OpenAI-compatible
  wire-JSON boundaries
  because they consume attacker-adjacent input (a compromised or buggy server
  must not crash the host app). Reflection decoding has deterministic boundary
  tests but no claimed property/fuzz gate in M3. All three fuzz targets run
  with long budgets in the scheduled nightly workflow (ADR 0012); the
  deterministic golden, arbitrary-split, and boundary wire tests remain in
  the per-commit suites.
- Valgrind/memcheck occasionally as a differently-shaped net; not gating.

## 6. CI Pipeline Shape

Three rings, ordered by feedback speed; a failure in an inner ring stops the outer ones:

1. **Per-commit (< ~10 min):** format check, tidy, build matrix (supported
   GCC 16 component with reflection ON; stable GCC/Clang with reflection OFF —
   the severability proof), unit + component tests, deterministic
   fake-transport and local-loopback integration tests, adapter golden suites,
   ASan/UBSan/TSan suites, and complexity gates. The M3 reflection leg also
   installs to a clean prefix and builds/runs a downstream
   `find_package(scry CONFIG REQUIRED COMPONENTS reflection)`
   consumer. clang-p2996 is manual, non-gating compatibility work and never
   builds installable or release artifacts.
2. **Nightly:** deep static analysis (CodeQL), long fuzz on all three
   protocol targets, and the M5 showcase contract — a default-OFF leg that
   enables the examples, builds them with warnings as errors, runs
   deterministic NPC and fake-panel cases, executes a real Dear ImGui
   headless frame, and repeats the core package-absence audit. Showcase
   feedback is immediate when it breaks (§8), so it does not gate PRs
   (ADR 0012).
3. **On demand (`workflow_dispatch`):** a bounded end-to-end smoke against a
   checksum-pinned local OpenAI-compatible server. The smoke uses a health
   check, hard startup/turn/job timeouts, one chat case and one tool round,
   and retained diagnostics on failure. It exercises a live model, not the
   deterministic protocol seams, so it never enters a gating ring.

**Everything CI does is orchestrated by one local command**
(`./scripts/preflight.sh`; `just ci` is an optional wrapper).
`just ci-fast` remains the optional wrapper for the quick core ring. The full
command runs every leg, continues after failures, and identifies toolchains
that the host cannot provide; hosted CI is authoritative for those
environments. A gate with no local entry point is a gate you learn about only
by pushing, which breeds resentment and workarounds — even solo.

The shared `scripts/ci-reflection.sh` gate is called by local preflight and
hosted CI. It performs a fresh GCC 16/P2996 build, runs the full configured
suite (27 reflection-labelled tests: 22 runtime/schema/codec/bridge cases plus
five compile-fail diagnostics), audits and consumes a clean reflection
install, then reruns the reflection tests in a separate ASan+UBSan build.
The core gate separately audits a
clean reflection-OFF install and downstream consumer. M3 evidence does not
claim a manual clang-p2996 run, randomized reflection property generation, or
a reflection fuzz target.

M4's per-commit evidence is live. The current development suite passes 277/277
tests, including exact OpenAI request/stream cases; endpoint, auth,
sampling, usage, error, lifecycle, fragmentation, and byte-limit matrices; the
fragmented transactional tool round; concurrent cross-dialect isolation; and
the public Curl path/header/SSE case. The provider slice passes 48/48 tests,
and `scry_openai_fuzz` joins the existing checked-corpus short fuzz ring. The
provider seam is streaming-only: the dead non-streaming decode path was
removed with its tests (see the evolution register in ARCHITECTURE.md §11).

ADR 0009 verification covers default and opted-in thread IDs, FIFO registration
and accepted-turn snapshots, all-worker and mixed batches, result
acknowledgements and cumulative budgets, exception/failure suppression,
cancellation, observer thread affinity, detached execute/resend/commit, queued
turns, and a bounded cooperating teardown handler under TSan. A deliberately
non-cooperating user handler remains outside Scry's enforceable shutdown bound
and is documented rather than represented by a hanging test.

The scheduled/manual `.github/workflows/nightly.yml` pipeline runs CodeQL
v4, long SSE/Anthropic/OpenAI fuzz jobs, and the showcase contract; the
bounded OpenAI-compatible smoke using checksum-pinned Ollama v0.32.1 pulling
`qwen3:1.7b-q4_K_M` is `workflow_dispatch`-only (ADR 0012). These are live
pipeline definitions; a completed hosted nightly execution is not yet
claimed. Separate local evidence is live: `scripts/ci-local-model.sh` passed
the public OpenAI-compatible chat and required-tool paths using Ollama 0.22.1
and `qwen3:1.7b-q4_K_M`. That local pass does not claim execution of the
checksum-pinned Ollama v0.32.1 hosted job.

M5 is live under ADR 0010. `scripts/ci-showcase.sh` is the single local/hosted
entry point: it runs 20 deterministic NPC, registration, and fake-controller
panel tests three times; compiles and executes one headless frame with the
pinned Dear ImGui sources under warnings-as-errors; and audits a clean
reflection-OFF install plus downstream consumer to prove that no showcase
artifact or dependency leaks into the package. The hosted nightly `showcase`
job and the local `just showcase` recipe both call that shared gate.

## 7. Workflow & Change Hygiene

- **Trunk-based; PRs even solo.** The PR is the unit of self-review: a forced read of the diff, a written description, and green gates before merge. Squash-merge, conventional-commit messages (the changelog is generated, not written).
- **Decisions get ADRs.** Anything that would surprise a future contributor — or future us — gets a short Architecture Decision Record in `docs/adr/`. The evolution register in ARCHITECTURE.md §11 is the standing index of "deliberately simple" decisions; ADRs capture the one-off forks in the road.
- **Definition of Done** for any change: gates green; docs updated if behavior or a decision changed (the load-bearing docs — including [REQUIREMENTS.md](REQUIREMENTS.md), the normative register — are not ceremonial; a stale doc is a bug); requirement rows updated to name their verifying test once it exists; public API changes come with a compiling example; deliberate simplifications added a row to the evolution register.
- **Dependency policy** (restating ARCHITECTURE.md §9 as process): new dependencies require a written justification committed with the change. Toolchains are pinned and upgraded deliberately — on this project the compilers are experimental, so "toolchain drift" is a first-class risk tracked like a dependency.

## 8. Solo-Project Pragmatism

Where the line sits between rigor and overhead, decided in advance:

- **Rigor is non-negotiable where bugs are silent:** threading (TSan), the machine and parsers (deterministic suites + nightly fuzz), API drift (golden files, package-consumer audits). These fail quietly in production and loudly in CI — that's the trade we're buying.
- **Pragmatism is fine where feedback is immediate:** example apps, demo polish, docs prose, CI plumbing itself. These fail visibly the moment they're wrong; gating them buys little.
- **Process weight is itself ratcheted — downward.** If a gate produces noise but never catches anything real for months, it gets demoted or deleted, with a note. The system of gates must stay credible, because the whole philosophy (§1) rests on actually trusting red to mean something. This clause has been exercised twice: [ADR 0011](docs/adr/0011-absolute-quality-gates.md) demoted the merge-base ratchet, the exact-exclusion reflection validator, the nightly mutation schedule, and the model manifest pin; [ADR 0012](docs/adr/0012-release-infrastructure-simplification.md) retired the coverage/CRAP gating machinery, mutation testing, and the feasibility spikes at the v0.0.1 release posture, and moved fuzzing and the showcase to the nightly ring.
