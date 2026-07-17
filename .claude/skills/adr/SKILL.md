---
name: adr
description: Create a new Architecture Decision Record in docs/adr/ following the project's established format. Use whenever a decision worth recording lands — build/tooling choices, design forks in the road, anything that would surprise a future contributor (ENGINEERING.md §7).
---

Create a new ADR in `docs/adr/`:

1. **Number it**: list `docs/adr/` and take the highest `NNNN` prefix + 1, zero-padded to four digits.
2. **Name it**: `NNNN-short-kebab-slug.md`.
3. **Use the template** in `template.md` (next to this skill file). Sections and order are fixed: title, Status/Date line, Context, Decision, Alternatives considered, Consequences.
4. **Status** is `accepted` unless told otherwise; **Date** is today in ISO form.
5. **Keep it under ~40 lines.** Each alternative gets one honest bullet: why it lost, and — where useful — a trigger that would reopen the decision.
6. **Register interplay**: if the decision changes or creates a requirement, update REQUIREMENTS.md in the same change (the register is normative) and cite the row ID in the ADR. If the decision is a deliberate simplification, it also needs an evolution-register row (ARCHITECTURE.md §11) — the ADR records *why*, the register row records *when to evolve*.
7. **Ship it with the change** that implements the decision, not as a separate commit.
