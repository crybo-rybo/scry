# Design Overview

> *Scrying: the practice of consulting an oracle by gazing into a mirror.*

A C++ LLM harness for applications with their own main loops. Scry's stable
C++23 surface turns explicit schemas and callables into LLM tools and hides the
full agentic loop — HTTP, streaming, tool dispatch, retries — behind a small,
poll-friendly API. An optional, experimental **C++26 reflection** component
derives the same registrations from ordinary C++ types.

The name is the design: **reflection** (the mirror) + **consulting an oracle** (the LLM). Namespace `scry::`; the canonical repository is [github.com/crybo-rybo/scry](https://github.com/crybo-rybo/scry).

---

## 1. Vision

Python has a dozen mature LLM harnesses. C++ has llama.cpp bindings for local inference and almost nothing for API-based integration — yet the applications that live in C++ (games, CAD, trading systems, embedded, desktop tools) are exactly the ones that can't easily shell out to Python.

Scry lets an existing C++ application add LLM capabilities — chat *and* tool use — by touching roughly five types, with zero changes to its threading or event architecture.

**Guiding principle:** tool use is the core design target, not an add-on. Chat is the degenerate case of an agentic loop with zero tools. The architecture is built around the loop from day one.

## 2. Goals and Non-Goals

**Goals**

- Drop-in integration for apps with their own main loop (game engines, GUI apps, simulation loops). No event loop assumed, none imposed.
- Tool registration with near-zero boilerplate via C++26 reflection (P2996):
  schema generation and argument marshalling derived from plain structs at
  compile time, lowering to the same C++23 registry. Optional and experimental.
- The harness owns the agentic loop entirely: model requests tool → harness executes it → result appended → resend → repeat until final answer.
- Provider abstraction at the message level, not the HTTP level: Anthropic and
  the documented OpenAI-compatible common subset for vLLM, Ollama, llama.cpp
  server, and LM Studio sit behind a config-only switch.
- Server/model configuration (base URL, auth, model, sampling params, optional
  reasoning disablement) as simple declarative config.
- Streaming, cancellation, and retries handled internally with clear thread guarantees.
- Examples that prove the public C++23 surface embeds in a real immediate-mode
  GUI and a small stateful game loop without expanding Scry's API or lifecycle.

**Non-Goals**

- Not an inference engine. Scry talks to servers; it does not load weights.
- Not a framework. Scry never owns `main()`, never spins an event loop the app must join, never demands ownership of app lifecycle.
- Not a GUI or game engine. Showcase views and world objects remain
  example-local; applications keep their window, rendering, input, state, and
  update-loop ownership.
- No prompt-template/chain DSL (LangChain-style). Apps compose in C++.
- MSVC support is deferred (no public P2996 support as of mid-2026; see
  [tools and providers](tools-and-providers.md)).

## 3. Target Environment

Primary: applications with a main loop that ticks at some frequency — game engines, Qt/ImGui/native GUI apps, simulators. Consequences that drive the whole design:

- **Never block by default.** Network turns take seconds; the main loop runs at 60 Hz or handles UI events. The async surface never waits on network I/O; the explicitly named `send_and_wait` convenience is reserved for CLI tools and tests.
- **Poll, don't push.** The app calls `scry::Harness::update()` once per tick. All callbacks fire inside `update()`, on the caller's thread. User code needs no locks.
- **Cancellation is normal.** Windows close, scenes change mid-request. Every in-flight turn has a `cancel()` safe to call at any time.
