#include "runtime/tool_registry_impl.hpp"
#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <scry/harness.hpp>
#include <scry/tool_registry.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(std::is_default_constructible_v<scry::ToolRegistry>);
static_assert(std::is_move_constructible_v<scry::ToolRegistry>);
static_assert(std::is_move_assignable_v<scry::ToolRegistry>);
static_assert(!std::is_copy_constructible_v<scry::ToolRegistry>);

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

TEST_CASE("tool manifests export the current registry without invoking handlers") {
  auto harness = scry::Harness::create(scry::test_support::test_config());
  REQUIRE(harness);
  const auto empty = harness->tools().to_json();
  REQUIRE(empty);
  CHECK(empty->text == R"({"tools":[],"version":1})");

  auto calls = std::make_shared<int>(0);
  REQUIRE(harness->tools().add(definition(), handler(calls)));
  const auto first = harness->tools().to_json();
  REQUIRE(first);
  CHECK(
      first->text ==
      R"({"tools":[{"description":"Get a forecast","input_schema":{"properties":{"city":{"description":"Place","type":"string"}},"required":["city"],"type":"object"},"name":"forecast"}],"version":1})");
  const auto repeated = harness->tools().to_json();
  REQUIRE(repeated);
  CHECK(repeated->text == first->text);

  REQUIRE_FALSE(harness->tools().add(definition(), handler(calls)));
  REQUIRE_FALSE(harness->tools().add(definition("invalid", "{"), handler(calls)));
  REQUIRE(harness->tools().add(definition("another", R"({"type":"object"})"),
                               handler(calls)));
  const auto later = harness->tools().to_json();
  REQUIRE(later);
  const auto parsed = scry::JsonView::parse(*later);
  REQUIRE(parsed);
  const auto tools = parsed->find("tools");
  REQUIRE(tools);
  REQUIRE(tools->size() == 2);
  CHECK(tools->at(0)->find("name")->string() == "forecast");
  CHECK(tools->at(1)->find("name")->string() == "another");
  CHECK(first->text.find("another") == std::string::npos);
  CHECK(empty->text == R"({"tools":[],"version":1})");
  CHECK(*calls == 0);
}

TEST_CASE("tool manifests preserve escaped metadata and nested schema values") {
  auto harness = scry::Harness::create(scry::test_support::test_config());
  REQUIRE(harness);
  const std::string name = "quoted\"tool\\name";
  const std::string description = "First line\nSecond\tline — café";
  REQUIRE(harness->tools().add(
      {
          .name = name,
          .description = description,
          .input_schema =
              {.text =
                   R"({"type":"object","properties":{"items":{"type":"array","items":{"enum":["a","b"]}},"limit":{"default":18446744073709551615}},"additionalProperties":false})"},
      },
      handler()));
  const auto manifest = harness->tools().to_json();
  REQUIRE(manifest);
  const auto parsed = scry::JsonView::parse(*manifest);
  REQUIRE(parsed);
  const auto tools = parsed->find("tools");
  REQUIRE(tools);
  REQUIRE(tools->size() == 1);
  const auto tool = tools->at(0);
  REQUIRE(tool);
  CHECK(tool->find("name")->string() == name);
  CHECK(tool->find("description")->string() == description);
  const auto schema = tool->find("input_schema");
  REQUIRE(schema);
  CHECK(schema->kind() == scry::JsonKind::object);
  CHECK(schema->find("additionalProperties")->boolean() == false);
  const auto properties = schema->find("properties");
  REQUIRE(properties);
  CHECK(properties->find("items")->find("items")->find("enum")->size() == 2);
  CHECK(properties->find("limit")->find("default")->unsigned_integer() ==
        std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("tool registration accepts only JSON object schemas and canonicalizes them") {
  scry::detail::ToolRegistryState state{};

  auto status = scry::detail::add_tool_registration(state, definition("malformed", "{"),
                                                    handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);

  status = scry::detail::add_tool_registration(
      state, definition("array", R"(["not","an","object"])"), handler());
  REQUIRE_FALSE(status);
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
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
  CHECK(status.error().category == scry::ErrorCategory::invalid_argument);
  REQUIRE(state.entries.size() == 1);
  CHECK(state.entries.front() == original);
}

TEST_CASE("tool registration argument failures report invalid_argument") {
  scry::detail::ToolRegistryState state{};

  auto empty_name =
      scry::detail::add_tool_registration(state, definition(""), handler());
  REQUIRE_FALSE(empty_name);
  CHECK(empty_name.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(empty_name.error().message == "tool name must not be empty");

  auto empty_handler =
      scry::detail::add_tool_registration(state, definition(), scry::ToolHandler{});
  REQUIRE_FALSE(empty_handler);
  CHECK(empty_handler.error().category == scry::ErrorCategory::invalid_argument);
  CHECK(empty_handler.error().message == "tool handler must not be empty");

  REQUIRE(scry::detail::add_tool_registration(state, definition(), handler()));
  auto duplicate = scry::detail::add_tool_registration(state, definition(), handler());
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().category == scry::ErrorCategory::invalid_argument);

  // invalid_state stays reserved for lifecycle failures. ToolRegistry's only
  // invalid_state path is an inactive handle, which a host reaches by moving a
  // registry away - covered by the moved-from case below.
  static_assert(std::is_move_constructible_v<scry::ToolRegistry>);
}

TEST_CASE("tool snapshots retain immutable registrations across later additions") {
  scry::detail::ToolRegistryState state{};
  auto calls = std::make_shared<int>(0);
  REQUIRE(scry::detail::add_tool_registration(state, definition(), handler(calls)));

  auto snapshot = scry::detail::snapshot_tools(state);
  REQUIRE(snapshot.entries->size() == 1);
  REQUIRE(scry::detail::add_tool_registration(
      state, definition("current_time", R"({"type":"object"})"), handler()));
  CHECK(state.entries.size() == 2);
  CHECK(snapshot.entries->size() == 1);
  CHECK(snapshot.entries->front() == state.entries.front());

  const auto& schemas = *snapshot.schemas;
  REQUIRE(schemas.size() == 1);
  CHECK(schemas.front().name == "forecast");
  CHECK(schemas.front().description == "Get a forecast");
  CHECK(schemas.front().input_schema.text ==
        snapshot.entries->front()->definition.input_schema.text);

  state.entries.clear();
  auto result = (*snapshot.entries->front()->handler)(
      scry::Json{.text = R"({"city":"Detroit"})"});
  REQUIRE(result);
  CHECK(result->text == R"({"city":"Detroit"})");
  CHECK(*calls == 1);
}

TEST_CASE("registry snapshots rebuild only when registration changed") {
  scry::detail::ToolRegistryState state{};
  REQUIRE(scry::detail::add_tool_registration(state, definition(), handler()));

  const auto first = scry::detail::snapshot_tools(state);
  const auto shared = scry::detail::snapshot_tools(state);
  CHECK(first.entries == shared.entries);
  CHECK(first.schemas == shared.schemas);
  CHECK(first.entries->size() == 1);
  CHECK(first.schemas->size() == 1);

  REQUIRE(scry::detail::add_tool_registration(
      state, definition("current_time", R"({"type":"object"})"), handler()));
  const auto rebuilt = scry::detail::snapshot_tools(state);
  CHECK(rebuilt.entries != first.entries);
  CHECK(rebuilt.schemas != first.schemas);
  CHECK(rebuilt.entries->size() == 2);
  CHECK(rebuilt.schemas->size() == 2);
  CHECK(first.entries->size() == 1);
  CHECK(rebuilt.schemas->back().name == "current_time");
}

TEST_CASE("a standalone registry exports its manifest without a Harness") {
  // No Config, no libcurl, no worker: a registry is a plain value, so a build or
  // CI step can emit the tool contract from a program that never talks to a
  // provider.
  scry::ToolRegistry tools;
  CHECK(tools.empty());
  REQUIRE(tools.add(definition(), handler()));
  REQUIRE(tools.add(definition("another", R"({"type":"object"})"), handler()));
  CHECK(tools.size() == 2);
  CHECK(tools.contains("another"));
  CHECK(tools.names() == std::vector<std::string>{"forecast", "another"});

  const auto manifest = tools.to_json();
  REQUIRE(manifest);
  CHECK(
      manifest->text ==
      R"({"tools":[{"description":"Get a forecast","input_schema":{"properties":{"city":{"description":"Place","type":"string"}},"required":["city"],"type":"object"},"name":"forecast"},{"description":"Get a forecast","input_schema":{"type":"object"},"name":"another"}],"version":1})");
}

TEST_CASE("a Harness adopts a registry built before it and runs its handlers") {
  using namespace scry::test_support;

  auto calls = std::make_shared<int>(0);
  scry::ToolRegistry tools;
  REQUIRE(tools.add(definition(), handler(calls)));

  auto fake = std::make_unique<scry::test::FakeTransport>();
  fake->enqueue(scripted_exchange(
      anthropic_tool_stream({ToolUseBlock{
          .id = "call-a", .name = "forecast", .arguments = R"({"city":"Detroit"})"}}),
      "tool-request"));
  fake->enqueue(scripted_exchange(anthropic_text_stream("done"), "final-request"));
  auto harness = unwrap(scry::detail::HarnessTestAccess::create(
      test_config(), provider(), std::move(fake), 0, {}, std::move(tools)));

  CHECK(harness.tools().names() == std::vector<std::string>{"forecast"});
  // Registration stays open through tools() after create().
  REQUIRE(
      harness.tools().add(definition("another", R"({"type":"object"})"), handler()));
  CHECK(harness.tools().size() == 2);

  auto conversation = unwrap(scry::Conversation::create());
  bool finished = false;
  auto turn = harness.send(conversation, "what is the forecast?",
                           {
                               .on_finished =
                                   [&finished](scry::Result<scry::Completion> outcome) {
                                     finished = outcome.has_value();
                                   },
                           });
  REQUIRE(turn);
  REQUIRE(pump_until(harness, [&finished] { return finished; }));
  CHECK(*calls == 1);
}

TEST_CASE("a moved-from registry is inactive and reports invalid_state") {
  scry::ToolRegistry tools;
  REQUIRE(tools.add(definition(), handler()));

  const scry::ToolRegistry adopted{std::move(tools)};
  CHECK(adopted.size() == 1);
  CHECK(adopted.contains("forecast"));

  // NOLINTBEGIN(bugprone-use-after-move): the moved-from state is under test.
  CHECK(tools.size() == 0);
  CHECK(tools.empty());
  CHECK_FALSE(tools.contains("forecast"));
  CHECK(tools.names().empty());

  const auto added =
      tools.add(definition("another", R"({"type":"object"})"), handler());
  REQUIRE_FALSE(added);
  CHECK(added.error().category == scry::ErrorCategory::invalid_state);

  const auto manifest = tools.to_json();
  REQUIRE_FALSE(manifest);
  CHECK(manifest.error().category == scry::ErrorCategory::invalid_state);

  // Move-assigning a fresh registry makes the variable usable again.
  tools = scry::ToolRegistry{};
  // NOLINTEND(bugprone-use-after-move)
  REQUIRE(tools.add(definition(), handler()));
  CHECK(tools.size() == 1);
}
