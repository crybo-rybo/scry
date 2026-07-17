# Scry

> *Scrying: consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. Scry uses **C++26 reflection (P2996)** to turn plain C++ structs into LLM-callable tools, and hides the full agentic loop — HTTP, streaming, tool dispatch, retries — behind a small, poll-friendly, never-blocking API. The name is the design: reflection (the mirror) + consulting an oracle (the LLM).

Built for the apps that live in C++ — games, GUI tools, simulators — where you can't block a frame, can't shell out to Python, and want tool use, not just chat.

**Status:** design phase. No code yet; the documents below are the project.

## Documentation

| Document | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | High-level design: vision, goals/non-goals, the ~5 public concepts, interaction and threading model (with diagrams), reflection-based tool registration, provider abstraction, open questions, roadmap (M0–M5). **Start here.** |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the code is shaped: the C++ patterns and idioms each piece commits to — actor-model concurrency, sans-I/O state machine, type erasure + consteval codegen, PImpl, error-as-value — plus the evolution register documenting every deliberate simplification and its intended end state. |
| [ENGINEERING.md](ENGINEERING.md) | How we work: testing plan and pyramid, coverage and CRAP/complexity gating, static and dynamic analysis (sanitizers, fuzzing, mutation testing), CI shape, workflow, and the ratchet philosophy — metrics never regress. |
| [REQUIREMENTS.md](REQUIREMENTS.md) | **The normative register.** Every binding requirement as a numbered RFC-2119 row with milestone and verification method. When prose elsewhere conflicts with the register, the register wins. |

## Reading order

DESIGN.md → ARCHITECTURE.md → ENGINEERING.md, then REQUIREMENTS.md as the binding summary. The first three explain *why*; the register states *what holds*.
