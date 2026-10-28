# Scrydantic split: findings log

This log records how Scrydantic was carved out of Scry's reflection layer: the
couplings met, the decisions taken, and what a move to a repository of its own
still needs. It is written phase by phase, so later sections can revise
earlier ones.

## Phase log

### Phase 0: baseline

- Branch `refactor/scrydantic-split` from `main` @ 2e2553d.
- `./scripts/test.sh` (preset `dev`): 507/507 tests pass. The `ctest -N` list
  was saved for the final comparison (see "Test mapping" below).
- clang-format and clang-tidy 22.1.8 were installed from PyPI into a scratch
  virtual environment (`pip install 'clang-format==22.*' 'clang-tidy==22.*'`);
  the system tools are LLVM 18. Both scripts pass on the baseline.

### Phase 1: skeleton and boundary

- `scrydantic/CMakeLists.txt` is a project of its own (`project(scrydantic)`),
  added by the root with `add_subdirectory(scrydantic)`. Scry passes its
  options down as `SCRYDANTIC_BUILD_TESTS`, `SCRYDANTIC_BUILD_EXAMPLES`,
  `SCRYDANTIC_WARNINGS_AS_ERRORS`, and `SCRYDANTIC_SANITIZER`, so the `asan` and
  `tsan` presets instrument Scrydantic too.
- `include(CTest)` moved above `add_subdirectory(scrydantic)` in the root, so
  Scrydantic's tests register in the same ctest tree.
- Scrydantic has its own warning target (`scrydantic_project_options`), a copy
  of `scry_project_options`; neither is exported.
- `scripts/lint.sh` rejects any `#include` of `<scry/...>`, `<curl/...>`, or a
  Scry `src/` header under `scrydantic/`, and any `CURL::`/`Threads::` target
  there. `scripts/format.sh` covers `scrydantic/`.
- Scrydantic is versioned on its own, starting at 0.1.0. The root passes the
  version to `scryConfig.cmake`, which does `find_dependency(scrydantic 0.1.0)`.

### Phase 2: JSON core

- `src/kernel/json/{document,number,view}` moved to `scrydantic/src/json/`
  (namespace `scrydantic::detail::json`), and `include/scry/json.hpp` to
  `<scrydantic/json.hpp>` (namespace `scrydantic`). They build as
  `scrydantic_json` (alias and export name `scrydantic::json`), a static C++23
  library without `-freflection`, with `-Werror=c++26-extensions` like Scry's
  kernel. `scrydantic/src/json/core.hpp` is the core's copy of
  `kernel/kernel.hpp`: a reflection guard every core source includes first.
- `<scrydantic/error.hpp>` holds `Error { message, model_message }`,
  `Result<T>`, `Status`, and an inline `tool_error()`. `json.hpp` includes it.
  The plan put these in `json.hpp`; a header of their own lets the reflected
  layer and a consumer that only wants the error type include less, and
  `json.hpp` still brings them in.
- `JsonView::parse` returns `scrydantic::Result<JsonView>`, with
  `message == "JSON text is not valid"` and an empty `model_message`. Every
  Scry caller already replaced that error with its own (`protocol` for an SSE
  payload, `invalid_config` for a conversation document), so Scry's behavior did
  not change.
- `<scrydantic/detail/json_text.hpp>` is new: the category-free text operations
  (`validate_json`, `validate_json_object`, `json_root_is_object`,
  `canonicalize_json[_object]` returning `std::optional<Json>`,
  `make_json_error_object`, `canonical_json_number`). It is installed, because
  Scry needs it, but it is `detail`. `src/kernel/json/codec.{hpp,cpp}` stays in
  Scry as the category-attaching wrappers the plan asked for; the three
  operations that never fail are using-declarations there, so provider code did
  not change.
- `scry_kernel` links `scrydantic::json` PUBLIC, so the kernel's C++23 objects
  see the core and the kernel boundary in `kernel/kernel.hpp` and `lint.sh` now
  admits `<scrydantic/{error,json,detail/json_text}.hpp>` (and no longer
  `<scry/json.hpp>`, which is gone).
- `lint.sh` runs clang-tidy over `scrydantic/src/json/*.cpp` as C++23 with
  `-Iscrydantic/include -Iscrydantic/src`, and checks the core's own include
  boundary (standard library, `"json/..."`, and its three public headers).

Couplings met that the plan did not list:

- `src/kernel/transport/transport_policy.cpp` read provider error bodies with
  the private document type (`json::parse`, `json::Value::find`). It now uses
  the public `JsonView`, at the cost of one copy of an error body that the
  response policy already bounds. Nothing in Scry reaches the document type now.
- The golden-fixture prefix test walked every file under `tests/fuzz/corpus/`,
  Scry's provider, conversation, SSE, and response-policy seeds included, and
  `corpus.txt` pins them whole. Those 18 seeds were copied, byte for byte, to
  `scrydantic/tests/fuzz/corpus/<same path>`, so the archive and
  `corpus_prefixes.list` stay valid unchanged. They are frozen copies: a new Scry
  seed no longer joins the JSON prefix test. `scrydantic/.gitattributes` keeps
  the seeds byte-exact (the copied `.http` seeds are CRLF).
- LLVM's default `IncludeCategories` treats `"json/..."` as a third-party
  include (it is in the `gtest|gmock|isl|json` list), so clang-format moved the
  core's guard after the other includes. `scrydantic/.clang-format` drops `json`
  from that list and ranks `"json/core.hpp"` first. A separate repository needs
  this file anyway; it also needs a copy of `.clang-tidy`.
- `tests/fuzz/replay_main.cpp` was copied to `scrydantic/tests/fuzz/`; Scry's
  other fuzz replays still use the original.
- `cmake/ExpectCompileFailure.cmake` takes an optional
  `DEPENDENCY_INCLUDE_DIR`, because Scry's headers now include Scrydantic's.
- Scrydantic's tests declare Catch2 first under Scry (the same revision), and
  Scry's `FetchContent_Declare` then reuses it.

Test changes:

- Two test helpers changed shape only: `unwrap` in
  `tests/testing/typed_answer_tests.cpp` takes any `std::expected`, and
  `decode_value` in `tests/reflection/reflection_tests.cpp` converts the parse
  error (until phase 3 moves the codec).
- `scrydantic::Error` has no category. Where a moved test checked
  `error().category == invalid_argument` on a `JsonView::parse` failure, it now
  checks every field the error has (`message` and an empty `model_message`).
  Scry's wrappers, which do attach a category, got a test of their own:
  `runtime.the JSON codec wrappers give the core's verdict under the caller's
  category`.
- `json_codec_validation_tests.cpp` moved to Scrydantic and now checks the
  core's text entry points (the ones Scry's wrappers call) against the
  document, instead of Scry's wrappers.

### Phase 3: codec and schema

- Moved to `scrydantic/include/scrydantic/` (namespace `scrydantic`, internals
  `scrydantic::detail`): `annotations.hpp`, and `detail/{json_string, model,
  meta, schema, decode, encode}.hpp` from `scry/detail/reflection_*.hpp`.
  `reflection_codec.hpp` became `detail/failure.hpp`, so it is not confused with
  the public `codec.hpp`. `scry/reflection.hpp` became `<scrydantic/codec.hpp>`
  (`encode`, both `decode` overloads; the concepts and `schema_v` /
  `input_schema_v` come from the detail headers it includes).
- `reflection_json.hpp` and `src/reflection/json_bridge.cpp` are gone. The two
  bridge functions were there only to keep a third-party JSON type out of the
  headers; they are now inline over `<scrydantic/detail/json_text.hpp>`, so the
  C++26 target stays header-only (an INTERFACE library over `scrydantic::json`).
- Coupling 2 (errors) is cut in the codec: every codec failure is a
  `scrydantic::Error`, with the same `message` and `model_message` texts as
  before. The handler return rules (`expected_traits`, `is_acknowledgment`,
  `result_type_problem`) recognize `scrydantic::Result<T>` and
  `scrydantic::Status`. A reflected handler that still returns `scry::Status`
  now fails to compile with "... which is not void, scrydantic::Status, a
  SupportedValue, or a Result of one: scry::Status ... is not a supported
  reflected value", which names the fix.
- Coupling 4 (`scry::Json` special case in `kind_of`) needed nothing beyond the
  rename: it is `^^scrydantic::Json`, a type Scrydantic owns.
- `ToolHandlerFor` and `tool_handler_problem` moved with `meta.hpp`, so they
  became generic here rather than in phase 4: `ToolHandlerFor<Handler, Args,
  Context = void>`. A `void` context means "no context parameter". The
  not-invocable text is built from `display_string_of(^^const Context&)`, which
  GCC 16 prints as `const scry::ToolCallContext&`, so the `wrong-context-order`
  compile-fail substring did not change.
- `response_format<Answer>()` moved to `namespace scry`, in a new public header
  `<scry/response_format.hpp>`, which `harness.hpp` includes. Its validator and
  `Harness::ask()` turn a `scrydantic::Error` into
  `ErrorCategory::invalid_argument` (`scry::detail::answer_error`), which is
  what `reflection::decode` reported before.
- `<scry/scry.hpp>` includes `<scrydantic/scrydantic.hpp>`. That is a header
  include for convenience, not a re-export: no Scrydantic name is aliased into
  `scry`.
- For this phase only, `scry/detail/reflection_tools.hpp` and
  `reflection_registration.hpp` stayed in Scry on top of Scrydantic, through
  `using namespace` directives, and converted Scrydantic errors to
  `ErrorCategory::tool` where a reflected handler becomes a
  `ContextualToolHandler`. Phase 4 replaces both.

Surprise (GCC 16, but really C++ lookup): `detail::decode_arguments` called
`decode<Args>(*parsed)` unqualified. With `JsonView` now in namespace
`scrydantic`, argument-dependent lookup also found the public
`scrydantic::decode<T>(const JsonView&)`, and the call became ambiguous for
every argument type. Before the move `JsonView` lived in `scry`, while the
public `decode` lived in `scry::reflection`, so ADL never saw it. The call is
now `detail::decode<Args>(...)`. Any unqualified call in `scrydantic::detail`
that shares a name with a public `scrydantic` function taking a Scrydantic type
has the same hazard.

Test changes:

- Reflected handlers and fixtures in tests and examples return
  `scrydantic::Result`/`scrydantic::Status` and use `scrydantic::tool_error`
  (`reflection_tests.cpp`, `openai_integration_tests.cpp`, `live_smoke.cpp`,
  `examples/{tool_policy,toolbox,typed_values}.cpp`). Dynamic handlers
  (`add_dynamic`) and answer validators keep `scry::Result`/`scry::Status`.
- Category checks on a `scrydantic::Error` were replaced by checks of every
  field it has: `codec_tests.cpp` (three decode checks, one encode check) and
  `reflection_tests.cpp` (the malformed `decode_arguments` check, three encode
  checks, and "encoding a named reflected error preserves the caller's error",
  which now also proves `model_message` survives). Categories are still
  checked wherever Scry attaches one: the reflected handlers built by
  `make_tool_handler` (`ErrorCategory::tool`), and a new test,
  `reflection.response_format validators report a rejected answer as
  invalid_argument`.
- Renamed: `reflection.public decoding reports invalid_argument with path-based
  messages` is now `reflection.public decoding reports path-based messages`,
  because Scrydantic's decode reports no category.
- The three context-handler `static_assert`s pass `scry::ToolCallContext` as the
  concept's new `Context` argument.
- Compile-fail substrings changed only by the rename: `scry::reflection::X` to
  `scrydantic::X`, and `scry::Status` to `scrydantic::Status` in
  `unsupported-tool-result`. Three compile-fail sources that use
  `scry::ToolRegistry` include `<scry/tool_registry.hpp>` instead of the deleted
  `<scry/reflection.hpp>`.
- The README quickstart's one annotation line changed with
  `examples/quickstart.cpp`, because `readme-quickstart` requires the two to be
  equal.
- `PRESET=asan ./scripts/test.sh` at the end of this phase: 509/509 pass.
