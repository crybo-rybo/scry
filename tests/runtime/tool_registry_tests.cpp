#include "runtime/tool_registry_impl.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <scry/tool_registry.hpp>
#include <string>
#include <type_traits>

static_assert(!std::is_move_constructible_v<scry::ToolRegistry>);
static_assert(!std::is_move_assignable_v<scry::ToolRegistry>);

namespace {

[[nodiscard]] scry::ToolDefinition definition(
    std::string name = "forecast",
    std::string schema =
        R"({"type":"object","required":["city"],"properties":{"city":{"type":"string","description":"Place"}}})") {
  return scry::ToolDefinition{
      .name = std::move(name),
      .description = "Get a forecast",
      .input_schema = scry::Json{.text = std::move(schema)},
  };
}

[[nodiscard]] scry::ToolHandler
handler(const std::shared_ptr<int>& calls = std::make_shared<int>(0)) {
  return scry::ToolHandler{[calls](scry::Json input) -> scry::Result<scry::Json> {
    ++*calls;
    return input;
  }};
}

} // namespace

TEST_CASE("tool registration accepts only JSON object schemas and canonicalizes them") {
  scry::detail::ToolRegistryState state{};

  auto status = scry::detail::add_tool_registration(state, definition("malformed", "{"),
                                                    handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);

  status = scry::detail::add_tool_registration(
      state, definition("array", R"(["not","an","object"])"), handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);
  CHECK(state.entries.empty());

  REQUIRE(scry::detail::add_tool_registration(state, definition(), handler()));
  REQUIRE(state.entries.size() == 1);
  CHECK(
      state.entries.front()->definition.input_schema.text ==
      R"({"properties":{"city":{"description":"Place","type":"string"}},"required":["city"],"type":"object"})");
  CHECK(state.entries.front()->handler);
}

TEST_CASE("tool registration is additive and duplicate names do not replace records") {
  scry::detail::ToolRegistryState state{};
  REQUIRE(scry::detail::add_tool_registration(state, definition(), handler()));
  const auto original = state.entries.front();

  auto status = scry::detail::add_tool_registration(
      state, definition("forecast", R"({"type":"object","properties":{}})"), handler());

  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);
  REQUIRE(state.entries.size() == 1);
  CHECK(state.entries.front() == original);
}

TEST_CASE("tool snapshots retain immutable registrations across later additions") {
  scry::detail::ToolRegistryState state{};
  auto calls = std::make_shared<int>(0);
  REQUIRE(scry::detail::add_tool_registration(state, definition(), handler(calls)));

  auto snapshot = scry::detail::snapshot_tools(state);
  REQUIRE(snapshot.size() == 1);
  REQUIRE(scry::detail::add_tool_registration(
      state, definition("current_time", R"({"type":"object"})"), handler()));
  CHECK(state.entries.size() == 2);
  CHECK(snapshot.size() == 1);
  CHECK(snapshot.front() == state.entries.front());

  const auto schemas = scry::detail::snapshot_schemas(snapshot);
  REQUIRE(schemas.size() == 1);
  CHECK(schemas.front().name == "forecast");
  CHECK(schemas.front().description == "Get a forecast");
  CHECK(schemas.front().input_schema.text ==
        snapshot.front()->definition.input_schema.text);

  const auto cached = scry::detail::schema_snapshot(state);
  REQUIRE(cached);
  CHECK(cached->size() == 2);
  CHECK(scry::detail::schema_snapshot(state) == cached);

  state.entries.clear();
  auto result =
      (*snapshot.front()->handler)(scry::Json{.text = R"({"city":"Detroit"})"});
  REQUIRE(result);
  CHECK(result->text == R"({"city":"Detroit"})");
  CHECK(*calls == 1);
}

TEST_CASE("schema snapshots are reused until a registration is added") {
  scry::detail::ToolRegistryState state{};
  const auto empty = scry::detail::schema_snapshot(state);
  REQUIRE(empty);
  CHECK(empty->empty());
  CHECK(scry::detail::schema_snapshot(state) == empty);

  REQUIRE(scry::detail::add_tool_registration(state, definition(), handler()));
  const auto first = scry::detail::schema_snapshot(state);
  REQUIRE(first);
  REQUIRE(first->size() == 1);
  CHECK(first != empty);
  CHECK(scry::detail::schema_snapshot(state) == first);

  REQUIRE(scry::detail::add_tool_registration(
      state, definition("current_time", R"({"type":"object"})"), handler()));
  const auto second = scry::detail::schema_snapshot(state);
  REQUIRE(second);
  REQUIRE(second->size() == 2);
  CHECK(second != first);
  CHECK(first->size() == 1);
}
