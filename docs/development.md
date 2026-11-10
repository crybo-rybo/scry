# Developing Scry

How to build, test, and change Scry itself. To use Scry in an application,
read the [README](../README.md) and the [guide](guide.md) instead.

## Build and test

```sh
./scripts/test.sh                  # configure, build, parallel ctest (dev preset)
./scripts/test.sh -R 'runtime\.'   # the script sends extra arguments to ctest
PRESET=asan ./scripts/test.sh      # or PRESET=tsan
./scripts/format.sh --fix          # --check (the default) only verifies
./scripts/lint.sh                  # clang-tidy on the kernel, plus the repository rules
./scripts/test-e2e.sh              # live model; see the header of the script
```

The presets select `g++-16` and build into `build/<preset>` with Ninja. If your
compiler has a different name, set `CXX` for the scripts. The formatter is
clang-format and the linter is clang-tidy, version 22 or newer; older versions
format reflection annotations differently. Set `CLANG_FORMAT` and `CLANG_TIDY`
to use a specific binary. `.clangd` points editors at `build/dev`.

CI runs these scripts in the official `gcc:16.2.0` image: the format check and
the lint in one job, and `test.sh` with the `dev`, `asan`, and `tsan` presets
in another. The tests download Catch2. `tsan` runs only on Linux, because GCC
supplies no thread-sanitizer runtime on Apple Silicon.

## Source layout

| Path | Content |
|---|---|
| `include/scry/` | The public API. Each header compiles on its own, and a test checks that |
| `include/scry/testing/`, `testing/` | `scry::testing`, the scripted server. It uses only the public headers |
| `src/kernel/` | The code that parses untrusted bytes or calculates retry and transport policy: the SSE parser, retry delays, Scry's JSON text helpers, and the transport seam with its libcurl implementation |
| `src/core/` | The internal message model, the provider interface, and `scrydantic.hpp`, which is Scry's adapter over Scrydantic's codec |
| `src/machine/` | The turn machine. It does no I/O and reads no clock |
| `src/provider/` | The Anthropic and OpenAI-compatible adapters: request encoding and stream decoding |
| `src/runtime/` | The Harness, the worker, the queues, the pump, tool dispatch, the registry, and conversation persistence |
| `examples/` | The programs that the documentation shows. The build compiles each one |
| `extras/showcase/` | Larger programs with their own dependencies. The main build does not include them |

### The kernel is C++23

Each build compiles `src/kernel/` as C++23 without `-freflection`, and the
kernel links only Scrydantic's C++23 JSON core, `scrydantic::json`. The reason
is clang-tidy, which cannot parse reflection: this split keeps the code that
reads untrusted bytes available to it. The kernel can include only the
standard library, libcurl, other kernel headers, and these headers:
`<scry/error.hpp>`, `<scry/config.hpp>`, `<scry/turn_id.hpp>`,
`<scry/unique_function.hpp>`, `<scrydantic/error.hpp>`, and
`<scrydantic/json.hpp>`. `src/kernel/kernel.hpp` states the boundary, and
`scripts/lint.sh` checks it.

The rest of `src/` is C++26. The build archives the kernel objects into
`scry::scry`. The kernel is not a separate installed target.

## How Scry uses Scrydantic

Scrydantic is an external dependency with its own repository, tests, and
releases. Scry uses only its public API, and it carries no copy of the parser,
the codec, the schema generator, or the binding:

| Need | Scrydantic API |
|---|---|
| Tool discovery, schemas, argument decoding, and result encoding | `bind()`, `bind_handler()`, `input_schema_v` |
| Typed answers and provider stream payloads | `decode()`, with constraints and enumerations on the wire structs |
| Conversation persistence and tool manifests | `encode()` |
| Provider request bodies, which keep stored payload bytes and pre-spelled sampling numbers | `encode_verbatim()` |
| JSON validation, canonical text, string escapes, and number spelling in the C++23 kernel | `is_valid_json()`, `JsonView::to_json()`, `escape_json_string()`, `format_json_number()` |
| Compile-time diagnostics, worded for Scry's call sites | `<scrydantic/diagnostics.hpp>` |
| The wire keys of a message type, so that persistence requires every member | `keys_v` |
| The tags of a tagged variant, so that unknown Anthropic events are ignored | `tags_v` |
| Rejections that a host can read | `Error::issues`, copied into `scry::Error::issues` |

Two small files hold what Scry adds on top:

- `src/core/scrydantic.hpp` words a codec failure as a Scry diagnostic, for
  example `Conversation document at $.messages[0].role is a required member`.
  A decode that found several issues has one line for each.
- `src/kernel/json_text.hpp` gives Scrydantic's text-level JSON operations a
  Scry error category.

A change that needs new behavior from the codec, the schemas, the binding, or
the JSON core belongs in Scrydantic. Make it there, release it, and raise the
version that Scry requires. Do not work around it in Scry.

### Provider wire types

Request encoding describes each body as reflected wire structs. It writes them
directly to JSON text with `encode_verbatim()`, and it does not build a
document tree. The structs borrow what they write. Text is a
`std::string_view`, and tool-call arguments and tool input schemas are
`const Json&` members. One validation scan, which does no allocation, checks
each embedded payload. This includes tool results, which go as JSON strings.
Then the encoder uses the payload as canonical text. The turn machine, tool
dispatch, and registration already made this text canonical. Thus a retry or a
tool round encodes again only the frame of the request.

Scry does not make a body canonical after it writes it. Thus the body relies on
the codec, which writes keys in canonical order and strings with the canonical
escapes. The canonical writer spells `temperature` and `top_p` before the
encode. For small and large exponents, the shortest spelling of a double by the
codec is different from the canonical spelling (`1e-07` against `1E-7`).

Stream decoding parses each SSE `data` payload once and decodes it into event
types that carry `ignore_unknown`. For Anthropic, the event type is a tagged
variant keyed on `type`. For OpenAI, it is one chunk type. The protocol
lifecycle stays hand-written: event order, block indices, finish reasons, usage
accumulation, and the argument byte limit.

### The conversation document

The conversation document keeps its own semantic rules, such as nonempty text
and which role may carry which block. They are not constraints on the public
message types: the rules belong to a stored document, not to every `Message`
that a host builds or decodes, and `to_json()` applies them as well, while an
encode checks no constraint.

### Changing the Scrydantic version

`cmake/Scrydantic.cmake` holds the minimum version and the pinned revision that
CMake fetches. The README states the version under Requirements. For work on
both libraries at once, set `FETCHCONTENT_SOURCE_DIR_SCRYDANTIC` to your
Scrydantic checkout. `SCRY_SANITIZER` also instruments a fetched Scrydantic.

## Tests

`tests/CMakeLists.txt` lists the suites, their ctest prefixes, and the rules
that they follow. In short:

- Test what Scry does. Scrydantic tests its codec, its schemas, its decoding
  rules, and its JSON core in its own repository. A test here that calls only
  `scrydantic::` functions on plain types belongs there. `tests/tools/`
  covers what Scry adds to a reflected tool or a typed answer: registration,
  dispatch, the error category, and what the model and the host receive.
- The test build replays seed corpora for the SSE parser, the transport
  response policy, the provider stream decoders, and conversation persistence.
  These replays are ordinary tests, and the `asan` preset runs them with ASan
  and UBSan. No build does a coverage-guided search.
- The `readme-quickstart` test fails unless the quickstart block of the README
  is `examples/quickstart.cpp`, byte for byte. Change the file, then copy it
  into the block.
- The `package-smoke` test installs Scry to an isolated prefix and builds a
  consumer against the exported targets only.

## Release

The comment at the top of [CMakeLists.txt](../CMakeLists.txt) has the steps.
The CMake project version is the only place the release number lives.
