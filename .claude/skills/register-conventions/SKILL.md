---
name: register-conventions
description: Conventions for editing REQUIREMENTS.md, Scry's normative requirements register — ID scheme, verification cells, amendments, and evolution-register interplay. Load before touching REQUIREMENTS.md.
user-invocable: false
---

REQUIREMENTS.md is **normative**: where DESIGN.md, ARCHITECTURE.md, or ENGINEERING.md prose conflicts with it, the register wins. Editing rules:

## IDs and rows

- ID scheme is `SCRY-<AREA>-NNN` (abbreviated `<AREA>-NNN` in tables). IDs are **permanent**: withdrawn requirements are ~~struck through~~, never deleted, never reused.
- Every row carries: Level (RFC-2119 MUST/MUST NOT/SHOULD/MAY), Requirement text, Milestone (per DESIGN.md §12), Verification.
- A requirement with no credible verification path is a design smell — rework it, don't waive it.

## Verification cells

- Cells start as a *planned* verification method. The moment the verifying test or gate exists, update the cell to **name it** (file, CI job, or test suite) — the convention is a `**Live:**` prefix, e.g. `**Live:** ci.yml sanitizers job`.
- Partial liveness is stated explicitly (e.g., "cognitive live via .clang-tidy; cyclomatic (lizard) planned").

## Amendments

- Substantive changes to a row get a dated note — either inline ("amended 2026-07") or in the **Unratified / Known Gaps** section's amendment log. New gaps land in that section, not in prose docs.
- When a register change alters what prose docs say, harmonize the prose in the same commit (QA-012: a stale doc is a bug). Check DESIGN.md §10/§12 and ARCHITECTURE.md §8/§10 — they restate error categories, warning flags, and milestone scope.

## Interplay

- Deliberate simplifications: requirement text may reference the evolution register (ARCHITECTURE.md §11); the register row states what holds *now*, the evolution row states the trigger and end state. Both must exist.
- One-off decisions cite their ADR (docs/adr/NNNN) in the row or amendment note.
