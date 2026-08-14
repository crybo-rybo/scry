# Development: Principles and Testing

The [design documentation](../design/overview.md) says what Scry is, the
[architecture documentation](../architecture/overview.md) says how the code is
shaped, and this section explains how the project is engineered. Philosophy
comes before procedure; specific thresholds appear only where a gate needs a
number to be enforceable.

---

## 1. Governing Philosophy

**Quality uses the strongest credible enforcement available.** Objective,
repeatable properties run automatically on every commit; change-specific
judgment that cannot be reduced to a trustworthy mechanical gate is handled by
explicit PR self-review. Scry currently has a single maintainer, so neither
side may be implicit: automated gates must fail loudly, and review obligations
must be written into the definition of done. Habits are named as habits rather than
misrepresented as machine-enforced standards.

**The architecture was designed for testability — the process must cash that check.** The sans-I/O state machine, injectable transport, and pure parsers exist so the hardest logic can be tested deterministically. If coverage on those components is low, that's not a testing failure, it's an architecture violation.

**Walking skeleton first.** The first thing that "works" is a thread of execution through every layer — public API → machine → adapter → transport → local server — doing the simplest useful thing (one streaming chat turn). Depth comes after the skeleton stands. This front-loads discovery of the integration risks (threading contract, curl lifetime, SSE realities) while the codebase is still cheap to reshape.

**Main is always green, always releasable.** Trunk-based development, short-lived branches, no long-running feature branches. Anything not ready to be on main hides behind a build flag (as the reflection layer already must).

**Gates are behavioral, not actuarial.** What gates a change is the physics:
the compiler matrix with warnings-as-errors, the deterministic test suites,
ASan/UBSan/TSan, clang-tidy, cyclomatic complexity, and the
install/package-consumer audits. Metric scoring — diff coverage, component
floors, CRAP — is not a gate. The
[evolution register](../architecture/quality-and-evolution.md)
records what would justify restoring any of that apparatus. Behavioral tests
remain while they cover live production behavior; a test may leave with the
implementation that was its only subject, but metric-driven deletion alone
remains out of bounds.

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
CI scores it; the standing habits do the work:

- **New behavior ships with tests at the sanctioned seam** (§2); a bug fix
  ships with its reproduction.
- **The pure components stay near-totally covered.** The machine, parsers, and
  classifiers were designed for deterministic testing; untested branches there
  are architecture violations, found by reading `llvm-cov report` when touching
  them.
- **Coverage-off pragmas require a comment.** Silent exclusion is the metric's
  death.
