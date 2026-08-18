# Development: Workflow

## 7. Workflow & Change Hygiene

- **Trunk-based; every change lands through a PR.** The PR is the unit of review: a forced read of the diff, a written description, and green gates before merge. Squash-merge, conventional-commit messages.
- **Decisions update the load-bearing docs.** Anything that would surprise a future contributor is recorded in the relevant design, architecture, development, or requirement section. The [evolution register](../architecture/quality-and-evolution.md) is the standing index of deliberately simple decisions.
- **Definition of Done** for any change: gates green; docs updated if behavior
  or a decision changed (the load-bearing docs — including the
  [requirements register](../requirements.md), which is normative — are not
  ceremonial; a stale doc is a bug); public API changes come with a compiling
  example; deliberate simplifications add a row to the evolution register. A
  performance or memory claim additionally includes compatible same-host
  parent/head measurements, unchanged semantic output, representative
  guardrails, and the affected cumulative comparison against the profiling
  foundation as required by QA-014 and the
  [profiling protocol](performance-profiling.md).
- **Dependency policy** (restating the [dependency architecture](../architecture/dependencies-and-errors.md) as process): new dependencies require a written justification committed with the change. Toolchains are pinned and upgraded deliberately — on this project the reflection compiler is experimental, so "toolchain drift" is a first-class risk tracked like a dependency.

## 8. Right-Sized Process

Where the line sits between rigor and overhead, decided in advance:

- **Rigor is non-negotiable where bugs are silent:** threading (TSan), the machine and parsers (deterministic suites + scheduled fuzz), API drift (golden files, package-consumer audits). These fail quietly in production and loudly in CI — that is the deliberate trade.
- **Pragmatism is fine where feedback is immediate:** example apps, demo polish, docs prose, CI plumbing itself. These fail visibly the moment they're wrong; gating them buys little.
- **Process weight is itself ratcheted — downward.** If a gate produces noise but never catches anything real for months, it gets demoted or deleted, with the active restoration trigger retained in the evolution register. The system of gates must stay credible, because the [governing philosophy](principles-and-testing.md) rests on actually trusting red to mean something.
