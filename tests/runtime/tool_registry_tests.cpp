#include "runtime/tool_registry_impl.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <scry/tool_registry.hpp>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

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
  scry::detail::CommandQueue commands{};

  auto status = scry::detail::add_tool_registration(
      state, commands, definition("malformed", "{"), handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);

  status = scry::detail::add_tool_registration(
      state, commands, definition("array", R"(["not","an","object"])"), handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);
  CHECK(state.entries.empty());

  REQUIRE(
      scry::detail::add_tool_registration(state, commands, definition(), handler()));
  REQUIRE(state.entries.size() == 1);
  CHECK(
      state.entries.front()->definition.input_schema.text ==
      R"({"properties":{"city":{"description":"Place","type":"string"}},"required":["city"],"type":"object"})");
  CHECK(state.entries.front()->execution == scry::ToolExecution::app_thread);
  CHECK(commands.size() == 0);
}

TEST_CASE("tool registration is additive and duplicate names do not replace records") {
  scry::detail::ToolRegistryState state{};
  scry::detail::CommandQueue commands{};
  REQUIRE(
      scry::detail::add_tool_registration(state, commands, definition(), handler()));
  const auto original = state.entries.front();

  auto status = scry::detail::add_tool_registration(
      state, commands, definition("forecast", R"({"type":"object","properties":{}})"),
      handler());

  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);
  REQUIRE(state.entries.size() == 1);
  CHECK(state.entries.front() == original);
}

TEST_CASE("tool snapshots retain immutable registrations across later additions") {
  scry::detail::ToolRegistryState state{};
  scry::detail::CommandQueue commands{};
  auto calls = std::make_shared<int>(0);
  REQUIRE(scry::detail::add_tool_registration(state, commands, definition(),
                                              handler(calls)));

  auto snapshot = scry::detail::snapshot_tools(state);
  REQUIRE(snapshot.size() == 1);
  REQUIRE(scry::detail::add_tool_registration(
      state, commands, definition("current_time", R"({"type":"object"})"), handler()));
  CHECK(state.entries.size() == 2);
  CHECK(snapshot.size() == 1);
  CHECK(snapshot.front() == state.entries.front());

  const auto schemas = scry::detail::snapshot_schemas(snapshot);
  REQUIRE(schemas.size() == 1);
  CHECK(schemas.front().name == "forecast");
  CHECK(schemas.front().description == "Get a forecast");
  CHECK(schemas.front().input_schema.text ==
        snapshot.front()->definition.input_schema.text);

  state.entries.clear();
  auto result =
      (*snapshot.front()->handler)(scry::Json{.text = R"({"city":"Detroit"})"});
  REQUIRE(result);
  CHECK(result->text == R"({"city":"Detroit"})");
  CHECK(*calls == 1);
}

TEST_CASE("worker registration transfers its handler through the command queue") {
  scry::detail::ToolRegistryState state{};
  scry::detail::CommandQueue commands{};
  auto calls = std::make_shared<int>(0);

  REQUIRE(scry::detail::add_tool_registration(
      state, commands, definition(), handler(calls),
      {.execution = scry::ToolExecution::worker_thread}));

  REQUIRE(state.entries.size() == 1);
  CHECK(state.entries.front()->execution == scry::ToolExecution::worker_thread);
  CHECK_FALSE(state.entries.front()->handler);
  CHECK(scry::detail::snapshot_worker_tool_names(state.entries) ==
        std::vector<std::string>{"forecast"});

  auto command = commands.try_pop();
  REQUIRE(command);
  auto* registration = std::get_if<scry::detail::RegisterWorkerToolCommand>(&*command);
  REQUIRE(registration);
  CHECK(registration->name == "forecast");
  auto result = registration->handler(scry::Json{.text = R"({"city":"Detroit"})"});
  REQUIRE(result);
  CHECK(*calls == 1);
}

TEST_CASE("worker registration precedes a subsequent accepted-turn command") {
  scry::detail::ToolRegistryState state{};
  scry::detail::CommandQueue commands{};
  REQUIRE(scry::detail::add_tool_registration(
      state, commands, definition(), handler(),
      {.execution = scry::ToolExecution::worker_thread}));
  commands.push(scry::detail::SendTurnCommand{
      .turn_id = {.value = 17},
      .worker_tool_names = scry::detail::snapshot_worker_tool_names(state.entries),
  });

  auto command = commands.try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::RegisterWorkerToolCommand>(*command));
  command = commands.try_pop();
  REQUIRE(command);
  const auto* send = std::get_if<scry::detail::SendTurnCommand>(&*command);
  REQUIRE(send);
  CHECK(send->worker_tool_names == std::vector<std::string>{"forecast"});
}

TEST_CASE("invalid worker execution policy leaves registry and queue unchanged") {
  scry::detail::ToolRegistryState state{};
  scry::detail::CommandQueue commands{};

  const auto status = scry::detail::add_tool_registration(
      state, commands, definition(), handler(),
      {.execution = static_cast<scry::ToolExecution>(255)});

  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_state);
  CHECK(state.entries.empty());
  CHECK(commands.size() == 0);
}
