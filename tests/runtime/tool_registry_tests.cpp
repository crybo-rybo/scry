#include "runtime/tool_registry_impl.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <scry/tool_registry.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] scry::ToolDefinition definition(
    std::string name = "forecast",
    std::string schema =
        R"({"type":"object","required":["city"],"properties":{"city":{"type":"string","description":"Place"}}})") {
  return scry::ToolDefinition{
      .name = std::move(name),
      .description = "Get a forecast",
      .input_schema = scrydantic::Json{.text = std::move(schema)},
  };
}

[[nodiscard]] scry::ToolHandler
handler(const std::shared_ptr<int>& calls = std::make_shared<int>(0)) {
  return scry::ToolHandler{
      [calls](scrydantic::Json input) -> scry::Result<scrydantic::Json> {
        ++*calls;
        return input;
      }};
}

[[nodiscard]] scry::ContextualToolHandler
contextual_handler(const std::shared_ptr<int>& calls = std::make_shared<int>(0)) {
  return scry::ContextualToolHandler{
      [calls](const scry::ToolCallContext&,
              scrydantic::Json input) -> scry::Result<scrydantic::Json> {
        ++*calls;
        return input;
      }};
}

// Registrations hold one handler shape, so every direct invocation in this suite
// names the call it stands in for.
[[nodiscard]] scry::Result<scrydantic::Json>
invoke(const scry::detail::RegisteredTool& tool, std::string arguments) {
  return tool.handler(scry::ToolCallContext{.tool_name = tool.definition.name},
                      scrydantic::Json{.text = std::move(arguments)});
}

// The registrations a Harness would freeze for its next turn.
[[nodiscard]] scry::detail::ToolSnapshot entries(scry::ToolRegistry& registry) {
  return *scry::detail::ToolRegistryAccess::snapshot(registry).entries;
}

} // namespace

TEST_CASE("tool registration accepts only JSON object schemas and canonicalizes them") {
  scry::ToolRegistry registry;

  auto status =
      registry.add_dynamic(definition("malformed", "{"), contextual_handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(status.error().message ==
        "tool input schema must be a valid JSON object: JSON text is not valid: "
        "unexpected end of input at line 1, column 2");

  status = registry.add_dynamic(definition("array", R"(["not","an","object"])"),
                                contextual_handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);

  status = registry.add_dynamic(definition("empty", ""), contextual_handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(registry.empty());

  REQUIRE(registry.add_dynamic(definition(), contextual_handler()));
  const auto registered = entries(registry);
  REQUIRE(registered.size() == 1);
  CHECK(
      registered.front()->definition.input_schema.text ==
      R"({"properties":{"city":{"description":"Place","type":"string"}},"required":["city"],"type":"object"})");
  CHECK(static_cast<bool>(registered.front()->handler));
}

TEST_CASE("tool registration is additive and duplicate names do not replace records") {
  scry::ToolRegistry registry;
  REQUIRE(registry.add_dynamic(definition(), contextual_handler()));
  const auto original = entries(registry).front();

  auto status = registry.add_dynamic(
      definition("forecast", R"({"type":"object","properties":{}})"),
      contextual_handler());

  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  const auto registered = entries(registry);
  REQUIRE(registered.size() == 1);
  CHECK(registered.front() == original);
}

TEST_CASE("tool registration argument failures report invalid_argument") {
  scry::ToolRegistry registry;

  auto empty_name = registry.add_dynamic(definition(""), contextual_handler());
  REQUIRE_FALSE(empty_name);
  CHECK(empty_name.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(empty_name.error().message == "tool name must not be empty");

  // An empty plain handler adapts to an empty contextual one rather than to a
  // wrapper that would throw on the first call, so both spellings are rejected
  // at registration.
  auto empty_handler = registry.add_dynamic(definition(), scry::ToolHandler{});
  REQUIRE_FALSE(empty_handler);
  CHECK(empty_handler.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(empty_handler.error().message == "tool handler must not be empty");

  auto empty_contextual =
      registry.add_dynamic(definition(), scry::ContextualToolHandler{});
  REQUIRE_FALSE(empty_contextual);
  CHECK(empty_contextual.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(empty_contextual.error().message == "tool handler must not be empty");

  REQUIRE(registry.add_dynamic(definition(), contextual_handler()));
  auto duplicate = registry.add_dynamic(definition(), contextual_handler());
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(registry.size() == 1);
}

TEST_CASE("tool snapshots retain immutable registrations across later additions") {
  auto calls = std::make_shared<int>(0);
  scry::detail::FrozenToolSnapshot snapshot{};
  {
    scry::ToolRegistry registry;
    REQUIRE(registry.add_dynamic(definition(), contextual_handler(calls)));

    snapshot = scry::detail::ToolRegistryAccess::snapshot(registry);
    REQUIRE(snapshot.entries->size() == 1);
    REQUIRE(registry.add_dynamic(definition("current_time", R"({"type":"object"})"),
                                 contextual_handler()));
    const auto current = scry::detail::ToolRegistryAccess::snapshot(registry);
    CHECK(current.entries->size() == 2);
    REQUIRE(current.schemas->size() == 2);
    CHECK(current.schemas->back().name == "current_time");
    CHECK(snapshot.entries->size() == 1);
    CHECK(snapshot.entries->front() == current.entries->front());
  }

  const auto& schemas = *snapshot.schemas;
  REQUIRE(schemas.size() == 1);
  CHECK(schemas.front().name == "forecast");
  CHECK(schemas.front().description == "Get a forecast");
  CHECK(schemas.front().input_schema.text ==
        snapshot.entries->front()->definition.input_schema.text);

  // The registry is gone; the snapshot alone keeps its registration callable.
  auto result = invoke(*snapshot.entries->front(), R"({"city":"Detroit"})");
  REQUIRE(result);
  CHECK(result->text == R"({"city":"Detroit"})");
  CHECK(*calls == 1);
}

TEST_CASE("a standalone registry exports its manifest without a Harness or its "
          "handlers") {
  // No Config, no libcurl, no worker: a registry is a plain value, so a build or
  // CI step can emit the tool contract from a program that never talks to a
  // provider.
  scry::ToolRegistry tools;
  CHECK(tools.empty());
  const auto empty = tools.to_json();
  REQUIRE(empty);
  CHECK(empty->text == R"({"tools":[],"version":1})");

  auto calls = std::make_shared<int>(0);
  REQUIRE(tools.add_dynamic(definition(), handler(calls)));
  REQUIRE_FALSE(tools.add_dynamic(definition(), handler(calls)));
  REQUIRE_FALSE(tools.add_dynamic(definition("invalid", "{"), handler(calls)));
  REQUIRE(
      tools.add_dynamic(definition("another", R"({"type":"object"})"), handler(calls)));
  CHECK(tools.size() == 2);
  CHECK(tools.contains("another"));
  // Names match exactly, never by prefix.
  CHECK_FALSE(tools.contains("anoth"));
  CHECK_FALSE(tools.contains(""));
  CHECK(tools.names() == std::vector<std::string>{"forecast", "another"});

  const auto manifest = tools.to_json();
  REQUIRE(manifest);
  CHECK(
      manifest->text ==
      R"({"tools":[{"description":"Get a forecast","input_schema":{"properties":{"city":{"description":"Place","type":"string"}},"required":["city"],"type":"object"},"name":"forecast"},{"description":"Get a forecast","input_schema":{"type":"object"},"name":"another"}],"version":1})");
  const auto repeated = tools.to_json();
  REQUIRE(repeated);
  CHECK(repeated->text == manifest->text);
  CHECK(*calls == 0);
}

TEST_CASE("a moved-from registry is inactive and reports invalid_state") {
  scry::ToolRegistry tools;
  REQUIRE(tools.add_dynamic(definition(), handler()));

  const scry::ToolRegistry adopted{std::move(tools)};
  CHECK(adopted.size() == 1);
  CHECK(adopted.contains("forecast"));

  // NOLINTBEGIN(bugprone-use-after-move): the moved-from state is under test.
  CHECK(tools.size() == 0);
  CHECK(tools.empty());
  CHECK_FALSE(tools.contains("forecast"));
  CHECK(tools.names().empty());

  const auto added =
      tools.add_dynamic(definition("another", R"({"type":"object"})"), handler());
  REQUIRE_FALSE(added);
  CHECK(added.error().category == scry::ErrorCategory::invalid_state);

  const auto manifest = tools.to_json();
  REQUIRE_FALSE(manifest);
  CHECK(manifest.error().category == scry::ErrorCategory::invalid_state);

  // Move-assigning a fresh registry makes the variable usable again.
  tools = scry::ToolRegistry{};
  // NOLINTEND(bugprone-use-after-move)
  REQUIRE(tools.add_dynamic(definition(), handler()));
  CHECK(tools.size() == 1);
}
