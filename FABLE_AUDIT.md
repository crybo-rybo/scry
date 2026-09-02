# Scry Foundation Audit

Date: 2026-09-01
Baseline: `main` at `d418f20` (v0.2.0)
Scope: read-only review of source, tests, build, CI, packaging, docs, and public API. No code was changed.

## Method

- Every library source file under `src/` and `include/` was read directly.
- The test suite, CI workflows, scripts, and packaging were audited by three parallel reviewers; every finding below was re-verified against the code before inclusion.
- Two `UniqueFunction` claims were confirmed by compiling probes against the header.
- The `dev` preset was configured, built, and tested locally on macOS with warnings as errors.

| Tests | Result | Wall time |
|---|---|---|
| 274 | all pass | under 4 s |

## Verdict

The foundation is stronger than most pre-1.0 C++ libraries. The architecture described in the docs is the architecture in the code: the worker owns all I/O, the turn machine is sans-I/O, and every byte path is bounded. The weak spots are not in the design. They are in quality gates that do not actually gate, a handful of sharp edges on the public API, and test infrastructure that hides failures instead of reporting them. Nothing below requires an architectural change.

Code hygiene counters any worry about generated cruft: ten lint suppressions, zero TODOs, low comment density, and the complexity gates pass. Where generation shows is in test duplication and in docs that claim more than the tooling does.

## What holds up

- **The concurrency model is real.** Cross-thread state is exactly the two queues and the per-turn atomic. TSan runs the whole suite three times per pull request.
- **The machine suite is the strongest layer.** Full illegal-transition matrix per phase, monotonic-time diagnostics, shuffled terminal orderings, exact byte-budget boundaries.
- **Resource bounding is thorough.** Saturating arithmetic everywhere, hostile-input handling at every wire boundary, response and SSE size ceilings, a per-turn queue ledger.
- **The package boundary is tested, not asserted.** Per-header standalone compile, third-party include audit, reflection-leak audit on a core-only install, downstream consumer build.
- **Secret hygiene is deliberate.** Key redaction in error text, header-injection validation, sanitized provider detail, no default log file.

## Findings, ranked

### Tier 1: gates that do not gate, and latent hazards

All are small fixes.

Struck-through findings were addressed by the Tier 1 remediation change.

| # | Finding | Where |
|---|---|---|
| ~~1~~ | ~~UBSan is non-fatal. There is no `-fno-sanitize-recover` and no `UBSAN_OPTIONS` with `halt_on_error`, so undefined behavior prints and exits zero. The ASan+UBSan leg gates only ASan.~~ | ~~`CMakeLists.txt` `scry_enable_sanitizer`~~ |
| ~~2~~ | ~~The weekly fuzz runs with no sanitizer at all. The `fuzz` preset sets no `SCRY_SANITIZER`, so only hard crashes are caught. One-line fix.~~ | ~~`CMakePresets.json`~~ |
| ~~3~~ | ~~The loopback test server tears down as shutdown, join, close. On macOS, shutdown on a listening socket does not wake `accept`, so any transport or integration test where curl never connects becomes a ctest timeout with no message. Latent today because every current test connects.~~ | ~~`tests/support/transport/loopback_server.cpp:141`~~ |
| ~~4~~ | ~~`cancel-in-progress: true` also applies to pushes on main, so back-to-back merges cancel the previous merge's run and main can carry commits with no completed CI. Same pattern in the performance workflow.~~ | ~~`.github/workflows/ci.yml`~~ |
| ~~5~~ | ~~The reflection PR gate's path filter omits the core public headers and all of `src/`, which the bridge compiles against. A core header change that breaks the component merges untested until the weekly run.~~ | ~~`.github/workflows/reflection.yml`~~ |
| ~~6~~ | ~~Retry jitter is a hash of turn id and attempt, so every process retries with identical jitter. Good for tests, but it defeats the purpose of jitter across a fleet. Seed per Harness, keep it injectable.~~ | ~~`src/runtime/worker.cpp` `jitter_sample`~~ |

### Tier 2: library correctness and robustness

Struck-through findings were addressed by the Tier 2 remediation change. Items 9, 10, and 11 were addressed by the Tier 3 remediation change (diagnostics and transport).

| # | Finding | Where |
|---|---|---|
| ~~7~~ | ~~`UniqueFunction` rejects a non-void callable in a void signature with a hard compile error. Verified: a lambda returning `buf.append(c)` cannot be an `on_text_delta`. A null function pointer also produces a non-empty wrapper. Use `std::invoke_r` and treat null pointers as empty.~~ | ~~`include/scry/unique_function.hpp:95`~~ |
| 8 | There is no way to sever callbacks from a live turn. Dropping the handle keeps them deliverable, cancel still delivers `on_finished`, and the only escape is destroying the Harness. Any capture of a UI object by reference dangles if the UI dies first. The ImGui showcase reinvents a weak-pointer-plus-generation scheme to survive this, and every host will. This needs a design decision because ERR-003 currently mandates delivery. | `include/scry/turn.hpp`, `examples/imgui/chat_panel.cpp:46` |
| ~~9~~ | ~~Non-2xx response bodies are discarded and `Error` carries no HTTP status. A wrong model name yields "provider rejected the request" and nothing else. Both providers put a sanitized `error.type` in the body, and ERR-001 already allows sanitized provider detail. Add an `http_status` field and parse the type token.~~ | ~~`src/transport/transport_policy.cpp` `accept_status_line`~~ |
| ~~10~~ | ~~The only network bound is a total transfer timeout, 120 s by default, applied to the whole streaming response. There is no stall or idle timeout. Slow local models on long tool loops will hit it, and the only remedy is raising the total. Add an idle timeout via `CURLOPT_LOW_SPEED_TIME` and make the total optional.~~ | ~~`src/transport/curl_transport.cpp` `configure_easy`~~ |
| ~~11~~ | ~~`update()` can make zero progress forever with a small budget: ingest checks the deadline before the first pop and delivery checks before the first callback. A test enshrines this for zero budgets, but a tiny positive budget on a slow host starves the same way. Guarantee one ingest and one delivery per call.~~ | ~~`src/runtime/pump_state.cpp`~~ |
| ~~12~~ | ~~`SamplingConfig::max_tokens` is optional but `nullopt` is rejected by both validators. The type advertises a choice that always fails.~~ | ~~`src/runtime/config.cpp`~~ |
| ~~13~~ | ~~Argument-validation failures such as an empty tool name, a duplicate name, a bad schema, or an empty user message all report `invalid_state`, which the enum documents as "invalid for the object's current state". Add `invalid_argument` or use `tool`.~~ | ~~`src/runtime/tool_registry.cpp`~~ |
| ~~14~~ | ~~`Conversation::create` promises `invalid_config` on validation failure and validates nothing.~~ | ~~`src/runtime/conversation.cpp`~~ |

### Tier 3: API gaps hosts must work around

Thin additions over state the runtime already holds, not features.

Struck-through findings were addressed by the Tier 4 remediation change (API polish).

| # | Finding |
|---|---|
| ~~15~~ | ~~`Json` is a bare string with no read API. The canonical example validates arguments by comparing text to `{}`, the NPC example hand-concatenates JSON, and the header points typed users at a component that needs GCC 16. A minimal read-only view with kind, at, find, string, and number plus an escape helper closes this. The private `JsonView` in `include/scry/detail/reflection_json.hpp` already has the right shape. This is the single biggest ergonomic gap for C++23 users.~~ |
| ~~16~~ | ~~`Conversation` exposes only `empty`, `message_count`, and `to_json`. A chat UI must parse the document or mirror state to render history.~~ |
| ~~17~~ | ~~Missing thin queries: `Conversation::busy()`, `Turn::finished()`, `Harness::cancel(TurnId)`, `ToolRegistry::contains` and names, and a public `validate(const Config&)`. Today the only way to validate a settings dialog's config is to spin up a curl transport and a worker thread.~~ |
| ~~18~~ | ~~`on_tool_call` fires for handler errors but the payload has no result and no `is_error`, so a UI cannot show what a tool returned.~~ |
| ~~19~~ | ~~`Config` has no CA bundle path, no extra headers, and no proxy setting. The only TLS knob is verify on or off, so corporate CAs and self-signed local servers force verification off.~~ |
| ~~20~~ | ~~`send_and_wait` delivers other turns' callbacks while it waits and cannot be cancelled. Neither is documented.~~ |

### Tier 4: internal quality and test infrastructure

| # | Finding | Where |
|---|---|---|
| 21 | Json-as-text means every request re-parses every tool schema and every historical tool call and result, per attempt and per tool round, then re-serializes them. Correct but wasteful. Store parsed values in the neutral model and serialize once at the wire. Best done after item 15. | `src/provider/*_request.cpp` |
| 22 | The worker reads the steady clock directly and waits on real deadlines, so no runtime-tier test proves a nonzero backoff or a Retry-After is honored end to end. The retry tests use zero backoff or race a long wait against cancel. Inject a clock and waiter. | `src/runtime/worker.cpp` |
| 23 | Curl connect and transfer timeouts are configured in tests but never allowed to expire, so THR-017's claim that every blocking curl phase is bounded has no mechanical coverage. Four assertions compare against wall-clock limits and run under TSan three times. | `tests/integration/curl_harness_tests.cpp:282`, `tests/transport/curl_transport_tests.cpp:314` |
| 24 | `FakeTransport` state is written on the worker thread and read unsynchronized from the test thread. Five bespoke transports each re-add a mutex and a gate. The Anthropic SSE literal is retyped seven times. Widen the fake and add a harness fixture. | `tests/support/transport/fake_transport.hpp` |
| 25 | Fuzz targets are never built per commit, so they rot between weekly runs. Two corpus seeds are non-streaming documents the decoders reject immediately. There is no fuzz target for response-header policy or `Conversation::from_json`. | `tests/fuzz/corpus` |
| 26 | Golden fixtures are hand-synthesized with no provenance or capture recipe, and the OpenAI golden is an inline literal. The docs describe them as captured real payloads that are easy to re-capture. | `tests/fixtures` |
| 27 | clang-tidy is attached only to the core target. Tests, examples, and the reflection component are never analyzed, although the config names them. The tidy job builds Catch2 and every test binary for nothing. | `CMakeLists.txt` `CXX_CLANG_TIDY` |
| 28 | The tidy and sanitizer legs exist as three hand-synced copies in the workflow, the preflight script, and the justfile. | `.github/workflows/ci.yml`, `scripts/preflight.sh`, `justfile` |

### Tier 5: packaging and project hygiene

| # | Finding |
|---|---|
| 29 | Glaze is consumed through a raw include directory with no `FIND_PACKAGE_ARGS`, so vcpkg, Conan, and Homebrew cannot substitute a packaged copy. `EXCLUDE_FROM_ALL` on `FetchContent_Declare` needs CMake 3.28 but the minimum is 3.25; on older CMake, Glaze's install rules likely land in Scry's prefix. That last point is inferred from release notes, not executed. |
| 30 | No `POSITION_INDEPENDENT_CODE`, so the archive cannot be linked into a consumer's shared library or plugin, a common shape for the game and GUI audience. |
| 31 | The libc++ and experimental-library flags are exported as public usage requirements with no compiler guard. No public header uses `jthread`, so only the link flag needs to be public. |
| 32 | Actions are pinned to tags rather than SHAs, there is no Dependabot, no build caching, no `SECURITY.md`, no release workflow, and the version is hand-maintained in six places with two cross-checked. |
| 33 | The docs say IWYU runs in CI and that lizard warns at 10 and reports function length. Nothing runs IWYU, and lizard is invoked with the fail threshold only. |

### Tier 6: process weight

An observation rather than a defect. The profiling apparatus is large relative to what it protects.

| Piece | Size |
|---|---|
| `scripts/perf-compare.py`, no tests | 1708 lines |
| benchmark sources | about 3000 lines |
| library sources | about 10000 lines |

By the project's own policy it is opt-in and evidence-ineligible in CI, yet it is part of preflight. The evolution register says process weight ratchets downward. Either give the comparison script tests or ask whether it earns its keep before 1.0.

The docs are an asset, but a behavior change touches up to four documents, and item 33 shows the tax is already being paid imperfectly. Prefer enforcing each claim mechanically or deleting the claim.

## Suggested order of work

1. **Gate fixes, one PR, half a day.** Items 1 through 5. After this, red means something again.
2. **Small correctness.** Items 6, 7, 12, 13, 14. Each is a few lines plus a test.
3. **Diagnostics and transport.** Items 9, 10, 11. These need ERR and NET register rows.
4. **API polish.** Items 15 through 20, then rewrite `examples/main_loop.cpp` around the Json view. The example also declares the app after the harness while capturing it by reference, which is safe only because the loop exits first. Each addition needs a requirements row and a compiling example, per the definition of done.
5. **Test infrastructure.** Items 22 through 26.
6. **Packaging and CI.** Items 29 through 33.
7. **Design decision, then implement.** Item 8 needs an ERR-003 amendment before code.
8. **Later.** Item 21 once the Json view exists, and items 27 and 28 whenever the CI scripts are next touched.
