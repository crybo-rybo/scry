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

### Phase 4: tool discovery and binding

- `scry/detail/reflection_tools.hpp` moved to `scrydantic/detail/tools.hpp`
  (discovery and diagnostics) and `reflection_registration.hpp` to
  `scrydantic/detail/binding.hpp` (handler generation). `<scrydantic/tools.hpp>`
  is the public face. Couplings 1, 3, and 5 are cut: nothing in Scrydantic names
  `ToolCallContext`, `ToolEntry`, `ContextualToolHandler`, `UniqueFunction`,
  `events.hpp`, or `tool_registry.hpp`.
- Public binding API (all in `namespace scrydantic`):
  - `ToolFunction<Context = void>`: `std::move_only_function<Result<Json>(const
    Context&, Json)>`, or `Result<Json>(Json)` for `void`.
  - `BoundTool<Context = void> { name, description, input_schema (Json), invoke }`.
  - `bind_handler<Args, Context = void>(handler)`: the typed-lambda form that
    `ToolRegistry::add<Args>(metadata, handler)` uses.
  - `bind<^^entity, Context = void>()` for a function or namespace, and
    `bind<Context = void>(std::shared_ptr<Toolbox>)` for a toolbox. A null
    toolbox binds nothing; Scry keeps rejecting it with `invalid_argument` before
    it calls `bind`.
  - `describe<^^entity, Context = void>()` and `describe<Toolbox, Context =
    void>()`: descriptor-only access, `constexpr std::array<ToolDescriptor, N>`
    of `string_view`s into static storage, so a manifest can be built at compile
    time without a toolbox object.
  - `tools_of(entity, context)` and `toolbox_tools_of(type, context)`: the
    consteval functions behind the `Tools` defaults, public so a wrapper (Scry)
    can compute the default at its own call site and forward it.
  - `Toolbox<T, Context = void>` and `ToolHandlerFor<H, Args, Context = void>`.
- Deviation from the plan's sketch: the plan wrote `bind<Context, ^^entity>()`.
  The entity comes first here, `bind<^^entity, Context>()`, so the common
  context-free call is `bind<^^ns>()` rather than `bind<void, ^^ns>()`. The
  toolbox overload takes `Context` first because the toolbox type is deduced.
- The context is threaded through the consteval layer as a reflection
  (`std::meta::info context`, `^^void` for none), and `bound_tool` gained a
  `Context` template argument, because whether the leading parameter is the
  context or an argument decides the argument object. The context parameter is
  recognized as `add_lvalue_reference(add_const(dealias(context)))`, which GCC 16
  compares equal to the declared `const scry::ToolCallContext&`.
- Call-site keying is preserved: `ToolRegistry::add<Entity, Tools =
  [:scrydantic::tools_of(Entity, ^^ToolCallContext):]>()` computes the default
  at the host's call site and passes it to `scrydantic::bind<Entity,
  ToolCallContext, Tools>()`; `add(Toolbox&&)` forwards its own `Tools` as
  before. `reflection.multi-tu-a_then_b` and `-b_then_a` pass unchanged. The call
  helpers are now keyed on (function, context) instead of the function alone;
  the context type is the same in every unit, so the property holds.
- Diagnostics that named Scry's API now take it from the caller:
  `entity_tools_diagnostic<Entity, Context>(api)` prefixes `api` ("scry::
  ToolRegistry::add" from Scry, "scrydantic::bind" or "scrydantic::describe"
  from Scrydantic), so the `tool-on-non-function` substring did not change.
  `owned_toolbox_diagnostic` (about `ToolRegistry::add(Toolbox&&)`) moved into
  `scry/tool_registry.hpp`. Three texts were reworded to drop the registry:
  "the toolbox object the registry holds cannot call it" became "the toolbox
  object the tools are bound to cannot call it" (twice), and "is a type;
  register an object of it as a toolbox with add(std::shared_ptr<T>) or
  add(T&&)" became "is a type; bind an object of it as a toolbox instead". No
  test asserted them. "is a non-static member function; register an object of
  Lamp as a toolbox" was kept, because a test asserts it and it reads fine for
  both libraries.
- Scry side: `ToolRegistry` gained one private non-template member,
  `add_reflected(std::vector<scrydantic::BoundTool<ToolCallContext>>)`, in
  `tool_registry.cpp`. It wraps each `invoke` with
  `scry::detail::reflected_handler`, which turns a `scrydantic::Error` into
  `ErrorCategory::tool` (the only field dispatch reads is `model_message`, so
  dispatch behaves as before), and then calls `add_all`. Cost: a reflected call
  now goes through two type-erased wrappers (`UniqueFunction` around
  `std::move_only_function`) instead of one.
- `add<Args>(metadata, handler)` used to go through `add_dynamic`; it now builds
  one `BoundTool` and goes through `add_reflected`. Both end in `add_all`, which
  validates and canonicalizes the schema the same way.

Test changes:

- `reflection_tests.cpp` builds reflected handlers with a local
  `make_tool_handler<Args>()` that is `reflected_handler(bind_handler<Args,
  ToolCallContext>())`, the composition `ToolRegistry::add<Args>()` uses, so the
  existing `ErrorCategory::tool` checks on those handlers still hold.
- `scry::reflection::Toolbox<T>` became `scrydantic::Toolbox<T,
  scry::ToolCallContext>` in `tool_registration_tests.cpp` and
  `public_api_contract.cpp`.

### Phase 5: standalone proof

- `scrydantic/tests` now builds, with Scrydantic and Catch2 only:
  - `scrydantic_tests`: the codec tests moved from `tests/reflection/
    codec_tests.cpp` (they included only Scrydantic headers already), and new
    binding tests (`tools/tool_binding_tests.cpp`): namespace, single function,
    shared toolbox, typed handlers, a host-defined context, `void` context,
    failures, and compile-time `describe()`.
  - `scrydantic.multi-tu-a_then_b` / `-b_then_a`: a copy of Scry's multi-TU test
    on `scrydantic::bind` directly, so the call-site keying property is proven
    without Scry. It also invokes each unit's `echo` handler with that unit's
    parameter name.
  - Compile-fail tests: the four of Scry's that needed no Scry header
    (`duplicate-description`, `unsupported-schema-root`, `untagged-variant`,
    `duplicate-key`) moved, with their substrings unchanged, and two new ones
    for the generic binding: `bind-misplaced-tool` (the `scrydantic::bind<^^...>()`
    prefix) and `misplaced-context` (a context type after the first parameter,
    named from the reflection of a host-defined `Session`).
    `scrydantic/cmake/ExpectCompileFailure.cmake` is a copy of Scry's without
    the dependency include directory.
- `scrydantic/examples/standalone_tools.cpp` (ctest
  `scrydantic.example.standalone-tools`) binds a namespace and a toolbox,
  prints an MCP-style `tools/list` result (`name`, `description`,
  `inputSchema`), decodes two `tools/call` requests, dispatches them, and exits
  0 only when the listing and both results are the expected bytes. The MCP shapes
  are themselves reflected structs (`scrydantic::name{"inputSchema"}`,
  `scrydantic::Json` for the opaque arguments, a tagged variant for content).
- Link check: `ldd` of the example and of `scrydantic_tests` shows only
  libstdc++, libm, libgcc_s, and libc; the example's link line is its object and
  `libscrydantic_json.a`.
- `cmake -S scrydantic` on its own configures, builds, and passes its 60 tests
  (Catch2 fetched as under Scry), then installs `include/scrydantic/**`,
  `lib/libscrydantic_json.a`, and `lib/cmake/scrydantic/`. A scratch consumer
  with only `find_package(scrydantic 0.1 REQUIRED)` and
  `scrydantic::scrydantic` builds and runs; a second consumer with only
  `find_package(scry 0.7 REQUIRED COMPONENTS testing)` builds and registers a
  reflected namespace. Scry's install carries the Scrydantic package beside its
  own, and `scryConfig.cmake` does `find_dependency(scrydantic 0.1.0)`.
- `PRESET=asan ./scripts/test.sh` at the end of this phase: 521/521 pass.
