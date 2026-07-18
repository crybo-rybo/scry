#include "runtime/pump.hpp"
#include "runtime/tool_dispatch.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/tool_registry.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace {

[[nodiscard]] scry::ToolDefinition tool_definition(std::string name) {
  return {
      .name = std::move(name),
      .description = "test tool",
      .input_schema = {.text = "{}"},
  };
}

[[nodiscard]] scry::detail::ToolRegistrationPtr
registered_tool(std::string name, scry::ToolHandler handler) {
  return std::make_shared<const scry::detail::RegisteredTool>(
      scry::detail::RegisteredTool{
          .definition = tool_definition(std::move(name)),
          .handler = std::make_shared<scry::ToolHandler>(std::move(handler)),
      });
}

[[nodiscard]] scry::detail::ToolCallBlock tool_call(std::string name = "forecast",
                                                    std::string id = "call-1") {
  return {
      .id = std::move(id),
      .name = std::move(name),
      .arguments = {.text = R"({"z":2,"a":1})"},
  };
}

[[nodiscard]] scry::detail::ToolCallEvent
tool_event(const scry::TurnId turn_id, std::string name = "forecast",
           std::string id = "call-1",
           const std::size_t remaining = std::numeric_limits<std::size_t>::max()) {
  return {
      .turn_id = turn_id,
      .call = tool_call(std::move(name), std::move(id)),
      .remaining_exchange_bytes = remaining,
  };
}

[[nodiscard]] scry::detail::CompletionEvent
completion_event(const scry::TurnId turn_id) {
  return {
      .turn_id = turn_id,
      .exchange = {scry::detail::Message{
          .role = scry::detail::Role::assistant,
          .content = {scry::detail::TextBlock{.text = "done"}},
      }},
      .finish_reason = scry::FinishReason::completed,
  };
}

struct PumpFixture {
  std::shared_ptr<scry::detail::CommandQueue> commands{
      std::make_shared<scry::detail::CommandQueue>()};
  std::shared_ptr<scry::detail::EventQueue> events{
      std::make_shared<scry::detail::EventQueue>()};
  std::shared_ptr<scry::detail::ConversationState> conversation{
      std::make_shared<scry::detail::ConversationState>()};

  [[nodiscard]] std::shared_ptr<scry::detail::TurnRoute>
  route(const std::uint64_t id, scry::detail::ToolSnapshot tools,
        const std::size_t result_limit = 1024) const {
    return std::make_shared<scry::detail::TurnRoute>(
        scry::TurnId{.value = id}, std::make_shared<std::atomic<bool>>(false), commands,
        conversation, "question",
        scry::detail::TurnRouteOptions{
            .tools = std::move(tools),
            .max_tool_result_bytes = result_limit,
            .max_conversation_bytes = 1024,
        });
  }
};

} // namespace

TEST_CASE("tool dispatch canonicalizes successful handler results") {
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"( { "z": [2, 1], "a": {"y":true,"x":null} } )"};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);

  REQUIRE(result);
  CHECK(result->tool_call_id == "call-1");
  CHECK_FALSE(result->is_error);
  CHECK(result->result.text == R"({"a":{"x":null,"y":true},"z":[2,1]})");
}

TEST_CASE("tool dispatch turns an unknown tool into a model-visible error") {
  const auto result = scry::detail::dispatch_tool({}, tool_call("missing"), 1024);

  REQUIRE(result);
  CHECK(result->tool_call_id == "call-1");
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"model requested an unknown tool"})");
}

TEST_CASE("tool dispatch treats unavailable handlers as model-visible errors") {
  const auto check_unavailable = [](scry::detail::ToolRegistrationPtr tool) {
    const auto result =
        scry::detail::dispatch_tool({std::move(tool)}, tool_call(), 1024);
    REQUIRE(result);
    CHECK(result->tool_call_id == "call-1");
    CHECK(result->is_error);
    CHECK(result->result.text == R"({"error":"tool handler is unavailable"})");
  };

  SECTION("missing handler storage") {
    check_unavailable(std::make_shared<const scry::detail::RegisteredTool>(
        scry::detail::RegisteredTool{
            .definition = tool_definition("forecast"),
            .handler = nullptr,
        }));
  }
  SECTION("empty type-erased handler") {
    check_unavailable(std::make_shared<const scry::detail::RegisteredTool>(
        scry::detail::RegisteredTool{
            .definition = tool_definition("forecast"),
            .handler = std::make_shared<scry::ToolHandler>(),
        }));
  }
}

TEST_CASE("tool dispatch does not disclose handler-returned Error details") {
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return std::unexpected(scry::Error{
            .category = scry::ErrorCategory::tool,
            .message = "secret application message",
            .provider_detail = "secret provider detail",
        });
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  CHECK(result->result.text.find("secret") == std::string::npos);
}

TEST_CASE("tool dispatch contains standard and non-standard handler exceptions") {
  const auto check_exception = [](scry::ToolHandler handler) {
    const scry::detail::ToolSnapshot tools{
        registered_tool("forecast", std::move(handler))};
    const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);
    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  };

  SECTION("standard exception") {
    check_exception([](scry::Json) -> scry::Result<scry::Json> {
      throw std::runtime_error{"secret exception message"};
    });
  }
  SECTION("non-standard exception") {
    check_exception([](scry::Json) -> scry::Result<scry::Json> { throw 7; });
  }
}

TEST_CASE("tool dispatch turns invalid handler JSON into a bounded tool error") {
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = "{"};
      })};

  const auto result = scry::detail::dispatch_tool(tools, tool_call(), 1024);

  REQUIRE(result);
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"tool handler returned invalid JSON"})");
}

TEST_CASE("tool dispatch enforces the canonical result byte limit exactly") {
  constexpr std::string_view canonical = R"({"a":1})";
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"({"a":1})"};
      })};

  const auto exact = scry::detail::dispatch_tool(tools, tool_call(), canonical.size());
  REQUIRE(exact);
  CHECK(exact->result.text == canonical);

  const auto over =
      scry::detail::dispatch_tool(tools, tool_call(), canonical.size() - 1);
  REQUIRE_FALSE(over);
  CHECK(over.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("tool dispatch fails when even its generic error exceeds the limit") {
  constexpr std::string_view generic_error = R"({"error":"tool execution failed"})";

  const auto result =
      scry::detail::dispatch_tool({}, tool_call("missing"), generic_error.size() - 1);

  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("pump executes tool handlers on the update caller thread") {
  PumpFixture fixture;
  std::thread::id handler_thread{};
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_thread](scry::Json) -> scry::Result<scry::Json> {
        handler_thread = std::this_thread::get_id();
        return scry::Json{.text = "null"};
      })};
  const auto route = fixture.route(301, tools);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  const auto caller_thread = std::this_thread::get_id();
  CHECK(handler_thread == std::thread::id{});
  CHECK(pump.update({}).callbacks_delivered == 1);

  CHECK(handler_thread == caller_thread);
  REQUIRE(fixture.commands->try_pop());
}

TEST_CASE("pump queues a tool result before notifying the ToolCall observer") {
  PumpFixture fixture;
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route = fixture.route(302, tools);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  std::size_t queued_when_observed = 0;
  std::string observed_id;
  REQUIRE(route->register_tool([&](const scry::ToolCall& call) {
    queued_when_observed = fixture.commands->size();
    observed_id = call.id;
  }));
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  CHECK(queued_when_observed == 1);
  CHECK(observed_id == "call-1");
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  const auto* result = std::get_if<scry::detail::ToolResultCommand>(&*command);
  REQUIRE(result);
  REQUIRE(result->result);
  CHECK(result->result->result.text == R"({"ok":true})");
}

TEST_CASE("cancellation during a handler suppresses its result and later calls") {
  PumpFixture fixture;
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t first_calls = 0;
  std::size_t second_calls = 0;
  std::size_t observer_calls = 0;
  bool cancellation_requested = false;
  scry::detail::ToolSnapshot tools{
      registered_tool("first",
                      [&](scry::Json) -> scry::Result<scry::Json> {
                        ++first_calls;
                        cancellation_requested = route->cancel();
                        return scry::Json{.text = R"({"first":true})"};
                      }),
      registered_tool("second",
                      [&](scry::Json) -> scry::Result<scry::Json> {
                        ++second_calls;
                        return scry::Json{.text = R"({"second":true})"};
                      }),
  };
  route = fixture.route(303, std::move(tools));
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(route->register_tool(
      [&observer_calls](const scry::ToolCall&) { ++observer_calls; }));
  REQUIRE(fixture.events->push(tool_event(route->id(), "first", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "second", "call-2"), 1024));

  const auto stats = pump.update({});

  CHECK(stats.events_remaining == 0);
  CHECK(cancellation_requested);
  CHECK(first_calls == 1);
  CHECK(second_calls == 0);
  CHECK(observer_calls == 0);
  CHECK(fixture.commands->size() == 1);
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::CancelTurnCommand>(*command));
}

TEST_CASE("fatal tool result failure suppresses every later handler") {
  PumpFixture fixture;
  std::size_t first_calls = 0;
  std::size_t second_calls = 0;
  const scry::detail::ToolSnapshot tools{
      registered_tool("first",
                      [&first_calls](scry::Json) -> scry::Result<scry::Json> {
                        ++first_calls;
                        return scry::Json{.text = R"({"oversized":true})"};
                      }),
      registered_tool("second",
                      [&second_calls](scry::Json) -> scry::Result<scry::Json> {
                        ++second_calls;
                        return scry::Json{.text = "{}"};
                      }),
  };
  const auto route = fixture.route(306, tools, 2);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "first", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "second", "call-2"), 1024));

  const auto stats = pump.update({});

  CHECK(stats.events_remaining == 0);
  CHECK(first_calls == 1);
  CHECK(second_calls == 0);
  CHECK(fixture.commands->size() == 1);
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  const auto& result = std::get<scry::detail::ToolResultCommand>(*command).result;
  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::resource_limit);
}

TEST_CASE("cumulative result failure suppresses remaining calls in the batch") {
  PumpFixture fixture;
  std::array<std::size_t, 3> calls{};
  const auto handler = [&calls](const std::size_t index) {
    return [&calls, index](scry::Json) -> scry::Result<scry::Json> {
      ++calls[index];
      return scry::Json{.text = "{}"};
    };
  };
  const scry::detail::ToolSnapshot tools{
      registered_tool("first", handler(0)),
      registered_tool("second", handler(1)),
      registered_tool("third", handler(2)),
  };
  const auto route = fixture.route(307, tools);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  constexpr std::size_t two_results_minus_one = 17;
  REQUIRE(fixture.events->push(
      tool_event(route->id(), "first", "call-1", two_results_minus_one), 1024));
  REQUIRE(fixture.events->push(
      tool_event(route->id(), "second", "call-2", two_results_minus_one), 1024));
  REQUIRE(fixture.events->push(
      tool_event(route->id(), "third", "call-3", two_results_minus_one), 1024));

  const auto stats = pump.update({});

  CHECK(stats.events_remaining == 0);
  CHECK(calls == std::array<std::size_t, 3>{1, 1, 0});
  CHECK(fixture.commands->size() == 2);
  auto accepted = fixture.commands->try_pop();
  auto rejected = fixture.commands->try_pop();
  REQUIRE(accepted);
  REQUIRE(rejected);
  CHECK(std::get<scry::detail::ToolResultCommand>(*accepted).result.has_value());
  CHECK_FALSE(std::get<scry::detail::ToolResultCommand>(*rejected).result.has_value());
}

TEST_CASE("detached routes continue dispatching tool calls") {
  PumpFixture fixture;
  std::size_t handler_calls = 0;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route = fixture.route(304, tools);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  route->detach();
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  CHECK(handler_calls == 1);
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::ToolResultCommand>(*command));
}

TEST_CASE("a terminal route suppresses a previously buffered tool call") {
  PumpFixture fixture;
  std::size_t handler_calls = 0;
  std::size_t observer_calls = 0;
  bool completed = false;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route = fixture.route(305, tools);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(route->register_tool(
      [&observer_calls](const scry::ToolCall&) { ++observer_calls; }));
  REQUIRE(route->register_completion(
      [&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));
  CHECK(pump.update({.max_callbacks = 0}).events_remaining == 1);
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));

  const auto terminal = pump.update({});

  CHECK(terminal.callbacks_delivered == 1);
  CHECK(terminal.events_remaining == 0);
  CHECK(completed);
  CHECK(route->terminal());
  CHECK(handler_calls == 0);
  CHECK(observer_calls == 0);
  CHECK(fixture.commands->size() == 0);
  CHECK(pump.live_route_count() == 0);
}
