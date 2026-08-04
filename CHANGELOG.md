# Changelog

All notable changes to Scry are documented in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) with the
pre-1.0 caveat recorded in [REQUIREMENTS.md](REQUIREMENTS.md) PORT-007: 0.x
releases may include breaking changes, and every breaking change is called out
here.

## [Unreleased]

## [0.0.1] - 2026-08-04

The first release of Scry: a C++ LLM harness for applications with their own
main loops. The C++23 surface is complete and tested; nothing about it is yet
promised stable — see the pre-1.0 notes below.

### Added

- The five-concept C++23 public surface: `scry::Config`, `scry::Conversation`,
  `scry::ToolRegistry`, `scry::Turn`, and `scry::Harness`, with fallible
  factories returning `std::expected` and no third-party types in public
  headers.
- The streaming agentic tool loop, owned by the harness: model request,
  streamed output, tool dispatch, automatic resend of tool results, and the
  final answer, bounded by a configurable `max_tool_rounds`.
- The poll-driven threading contract: `send()` never waits on network I/O, and
  every callback and tool handler runs inside `update()` on the calling
  thread. One worker thread per Harness performs all blocking I/O.
- Explicit-schema tool registration: a `ToolDefinition` (name, description,
  JSON-object schema) plus a move-only `Json → Result<Json>` handler,
  snapshotted per accepted turn. Handler errors and exceptions become bounded
  model-visible tool errors.
- Two provider dialects selected from `Config` alone: Anthropic Messages, and
  a strict OpenAI-compatible Chat Completions subset covering OpenAI, vLLM,
  Ollama, llama.cpp server, and LM Studio, with authentication optional so
  local servers need no API key.
- Cooperative cancellation (`Turn::cancel()`), bounded retries with
  exponential backoff and jitter honoring `Retry-After`, configurable
  resource bounds on payloads and pending work, and TLS peer verification on
  by default.
- Transactional conversation history: success commits the full exchange
  atomically; failure and cancellation commit nothing. App-managed
  persistence via `Conversation::to_json()` / `from_json()` with a versioned,
  strictly validated document.
- One error model end-to-end: a single categorized `Error` value, reported
  through `std::expected` before a turn is accepted and through the single
  terminal `on_finished` callback afterwards.
- The explicitly named synchronous `send_and_wait()` convenience for CLI
  tools and tests.
- The optional, experimental `scry::reflection` component (GCC 16+,
  `-std=c++26 -freflection`): `scry::reflection::add<Args>()` derives a
  canonical compile-time schema and strict typed marshalling from plain
  aggregates and lowers onto the same registry.
- Opt-in build-time diagnostic logging (`-DSCRY_ENABLE_LOGGING=ON` plus the
  `SCRY_LOG_FILE` environment variable); credentials and prompt/tool content
  never reach the log.
- CMake packaging: `find_package(scry CONFIG)` with the optional `reflection`
  component, FetchContent-friendly top-level guards, and exported
  `scry::scry` / `scry::reflection` targets.
- Platform support: Linux and macOS with GCC 14, Clang 18 (libc++), and
  AppleClang; requires CMake ≥ 3.25 and libcurl ≥ 7.84.
- The canonical first program (`examples/main_loop.cpp`), opt-in showcase
  examples (Dear ImGui chat panel, NPC grid world), and a warning-clean
  Doxygen API reference.

### Pre-1.0 notes

- No API or ABI stability is promised before 1.0; breaking changes land with
  an entry in this file.
- The `Conversation` persistence document format is unstable before 1.0: a
  document written by one 0.x release is not guaranteed to load in another.
- Turns are serialized: one HTTP transfer is active at a time per Harness.
- The reflection component is experimental and supported on Linux only.
- Windows is not supported.

[Unreleased]: https://github.com/crybo-rybo/scry/compare/v0.0.1...HEAD
[0.0.1]: https://github.com/crybo-rybo/scry/releases/tag/v0.0.1
