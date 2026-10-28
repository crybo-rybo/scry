# Scry (unreleased)

The reflection layer of Scry is now a separate library, Scrydantic. It is in
this repository under `scrydantic/`, and Scry links it. Scrydantic has the
annotations, the JSON types, the strict codec, the schema generator, and the
tool binding. It does not need Scry, libcurl, or a model. Thus, you can use it
alone, for example to serve C++ tools over MCP.

The split is a clean break. The old names are removed, and there are no aliases.
Change the names in your code as the tables below show. The behavior does not
change: the schemas, the canonical JSON, the decode rules, and the text that the
model receives are the same.

## Breaking changes

### Headers

| Removed | Use instead |
|---|---|
| `<scry/json.hpp>` | `<scrydantic/json.hpp>` |
| `<scry/annotations.hpp>` | `<scrydantic/annotations.hpp>` |
| `<scry/reflection.hpp>` | `<scrydantic/codec.hpp>` for `encode()` and `decode()`; `<scry/response_format.hpp>` for `response_format<Answer>()` |
| `<scry/detail/reflection_*.hpp>` | Internal. The replacements are under `<scrydantic/detail/>`. |

`<scry/scry.hpp>` includes `<scrydantic/scrydantic.hpp>`, so a program that
includes only the umbrella header needs no new include.

### Names

| Removed | Use instead |
|---|---|
| `scry::Json`, `scry::JsonKind`, `scry::JsonView` | `scrydantic::Json`, `scrydantic::JsonKind`, `scrydantic::JsonView` |
| `scry::escape_json_string()` | `scrydantic::escape_json_string()` |
| `scry::reflection::description`, `description_of`, `name`, `tag`, `tool`, `skip_null`, `emit_null`, `ignore_unknown` | The same names in `scrydantic` |
| `scry::reflection::encode()`, `decode()` | `scrydantic::encode()`, `scrydantic::decode()` |
| `scry::reflection::schema_v`, `input_schema_v` | `scrydantic::schema_v`, `scrydantic::input_schema_v` |
| `scry::reflection::SupportedValue`, `Encodable`, `Decodable`, `ToolArguments` | The same names in `scrydantic` |
| `scry::reflection::ToolHandlerFor<H, Args>` | `scrydantic::ToolHandlerFor<H, Args, scry::ToolCallContext>` |
| `scry::reflection::Toolbox<T>` | `scrydantic::Toolbox<T, scry::ToolCallContext>` |
| `scry::reflection::response_format<Answer>()` | `scry::response_format<Answer>()` |

### Errors

- `scrydantic::encode()` and `scrydantic::decode()` return
  `scrydantic::Result<T>`. `scrydantic::Error` has `message` and
  `model_message` only, with the same texts as before. It has no category.
  `Harness::ask()` and the validator of `response_format()` still report a
  rejected answer as `ErrorCategory::invalid_argument`.
- `scrydantic::JsonView::parse()` returns `scrydantic::Result<JsonView>`.
- A reflected tool, whether a function, a toolbox member, or an `add<Args>()`
  handler, returns `scrydantic::Result<T>` or `scrydantic::Status` instead of
  `scry::Result<T>` or `scry::Status`, and refuses a call with
  `scrydantic::tool_error()`. A reflected tool that still returns a Scry result
  fails to compile, and the diagnostic gives the reason. Scry reports the error
  of a reflected tool as `ErrorCategory::tool`, as before.
- Dynamic tools (`add_dynamic()`) and answer validators do not change: they
  use `scry::Result`, `scry::Status`, and `scry::tool_error()`.

### Packaging

- The install has a second CMake package, `scrydantic`, with the targets
  `scrydantic::scrydantic` and `scrydantic::json`. `find_package(scry)` finds it.
- `scry::scry` links `scrydantic::scrydantic` PUBLIC.

## New

- `scrydantic::bind<^^function_or_namespace, Context>()`,
  `scrydantic::bind<Context>(std::shared_ptr<Toolbox>)`, and
  `scrydantic::bind_handler<Args, Context>(handler)` make `BoundTool` values: a
  name, a description, a generated input schema, and a handler. The call context
  is a type that you choose, or `void` for none. `ToolRegistry` uses these with
  `scry::ToolCallContext`.
- `scrydantic::describe<^^entity>()` and `scrydantic::describe<Toolbox>()` give
  the contracts at compile time, without handlers or a toolbox object.
- `scrydantic/examples/standalone_tools.cpp` lists reflected tools as an
  MCP-style `tools/list` result and serves `tools/call` requests without Scry.
