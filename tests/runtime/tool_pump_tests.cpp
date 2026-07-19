#include "tool_dispatch_test_support.hpp"

#include <array>
#include <thread>
#include <variant>
#include <vector>

using namespace scry::test_support;

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

TEST_CASE("pump gates later calls until a worker result is accepted") {
  PumpFixture fixture;
  std::size_t app_calls = 0;
  std::vector<std::string> observed;
  const scry::detail::ToolSnapshot tools{
      registered_tool(
          "worker",
          [](scry::Json) -> scry::Result<scry::Json> {
            return scry::Json{.text = "{}"};
          },
          scry::ToolExecution::worker_thread),
      registered_tool("app",
                      [&app_calls](scry::Json) -> scry::Result<scry::Json> {
                        ++app_calls;
                        return scry::Json{.text = "{}"};
                      }),
  };
  const auto route = fixture.route(308, tools);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(route->register_tool(
      [&observed](const scry::ToolCall& call) { observed.push_back(call.name); }));
  REQUIRE(fixture.events->push(tool_event(route->id(), "worker", "call-worker"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "app", "call-app"), 1024));

  const auto scheduled = pump.update({});
  CHECK(scheduled.callbacks_delivered == 1);
  CHECK(scheduled.events_remaining == 1);
  CHECK(app_calls == 0);
  CHECK(observed.empty());
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::ExecuteWorkerToolCommand>(*command));

  const scry::detail::ToolResultBlock result{
      .tool_call_id = "call-worker",
      .result = {.text = "{}"},
  };
  REQUIRE(fixture.events->push(
      scry::detail::WorkerToolAcceptedEvent{
          .turn_id = route->id(),
          .tool_call_id = "call-worker",
          .result_payload_bytes = scry::detail::content_payload_bytes(result),
      },
      1024));
  const auto accepted = pump.update({.max_callbacks = 1});
  CHECK(accepted.callbacks_delivered == 1);
  CHECK(accepted.events_remaining == 1);
  CHECK(observed == std::vector<std::string>{"worker"});
  CHECK(app_calls == 0);

  CHECK(pump.update({.max_callbacks = 1}).callbacks_delivered == 1);
  CHECK(observed == std::vector<std::string>{"worker", "app"});
  CHECK(app_calls == 1);
  command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::ToolResultCommand>(*command));
}

TEST_CASE("worker observer remains ordered before an already queued completion") {
  PumpFixture fixture;
  std::vector<std::string> timeline;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "worker",
      [](scry::Json) -> scry::Result<scry::Json> { return scry::Json{.text = "{}"}; },
      scry::ToolExecution::worker_thread)};
  const auto route = fixture.route(309, tools);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(route->register_tool(
      [&timeline](const scry::ToolCall&) { timeline.emplace_back("tool"); }));
  REQUIRE(route->register_completion(
      [&timeline](const scry::Completion&) { timeline.emplace_back("complete"); }));
  REQUIRE(fixture.events->push(tool_event(route->id(), "worker", "call-worker"), 1024));
  CHECK(pump.update({}).callbacks_delivered == 1);
  REQUIRE(fixture.commands->try_pop());

  REQUIRE(fixture.events->push(
      scry::detail::WorkerToolAcceptedEvent{
          .turn_id = route->id(),
          .tool_call_id = "call-worker",
          .result_payload_bytes =
              scry::detail::content_payload_bytes(scry::detail::ToolResultBlock{
                  .tool_call_id = "call-worker",
                  .result = {.text = "{}"},
              }),
      },
      1024));
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 2);
  CHECK(timeline == std::vector<std::string>{"tool", "complete"});
}

TEST_CASE("worker acknowledgement advances cumulative result-budget accounting") {
  PumpFixture fixture;
  std::size_t app_calls = 0;
  std::vector<std::string> observed;
  const auto worker_result = scry::detail::ToolResultBlock{
      .tool_call_id = "call-worker",
      .result = {.text = "{}"},
  };
  const auto app_result = scry::detail::ToolResultBlock{
      .tool_call_id = "call-app",
      .result = {.text = "{}"},
  };
  const auto worker_bytes = scry::detail::content_payload_bytes(worker_result);
  const auto app_bytes = scry::detail::content_payload_bytes(app_result);
  const auto budget = worker_bytes + app_bytes - 1;
  const scry::detail::ToolSnapshot tools{
      registered_tool(
          "worker",
          []([[maybe_unused]] scry::Json value) -> scry::Result<scry::Json> {
            return {};
          },
          scry::ToolExecution::worker_thread),
      registered_tool("app",
                      [&app_calls](scry::Json) -> scry::Result<scry::Json> {
                        ++app_calls;
                        return scry::Json{.text = "{}"};
                      }),
  };
  const auto route = fixture.route(310, tools, 1024, budget);
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(route->register_tool(
      [&observed](const scry::ToolCall& call) { observed.push_back(call.name); }));
  REQUIRE(fixture.events->push(tool_event(route->id(), "worker", "call-worker", budget),
                               1024));
  REQUIRE(
      fixture.events->push(tool_event(route->id(), "app", "call-app", budget), 1024));

  CHECK(pump.update({.max_callbacks = 1}).callbacks_delivered == 1);
  REQUIRE(fixture.commands->try_pop());
  REQUIRE(fixture.events->push(
      scry::detail::WorkerToolAcceptedEvent{
          .turn_id = route->id(),
          .tool_call_id = "call-worker",
          .result_payload_bytes = worker_bytes,
      },
      1024));
  CHECK(pump.update({.max_callbacks = 1}).callbacks_delivered == 1);
  CHECK(observed == std::vector<std::string>{"worker"});

  CHECK(pump.update({.max_callbacks = 1}).callbacks_delivered == 1);
  CHECK(app_calls == 1);
  CHECK(observed == std::vector<std::string>{"worker"});
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  const auto* result = std::get_if<scry::detail::ToolResultCommand>(&*command);
  REQUIRE(result);
  REQUIRE_FALSE(result->result);
  CHECK(result->result.error().category == scry::ErrorCategory::resource_limit);
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

TEST_CASE("worker acknowledgements reject absent and mismatched pending calls") {
  PumpFixture fixture;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "worker", [](scry::Json) -> scry::Result<scry::Json> { return {}; },
      scry::ToolExecution::worker_thread)};
  const auto route = fixture.route(311, tools);
  const scry::detail::WorkerEvent accepted{scry::detail::WorkerToolAcceptedEvent{
      .turn_id = route->id(),
      .tool_call_id = "call-worker",
  }};

  CHECK_FALSE(route->has_callback(accepted));
  CHECK_FALSE(route->should_retain(accepted));
  CHECK(route->should_discard(accepted));
  route->invoke(accepted);

  route->invoke(
      scry::detail::WorkerEvent{tool_event(route->id(), "worker", "call-worker")});
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::ExecuteWorkerToolCommand>(*command));

  const scry::detail::WorkerEvent mismatched{scry::detail::WorkerToolAcceptedEvent{
      .turn_id = route->id(),
      .tool_call_id = "other-call",
  }};
  CHECK_FALSE(route->has_callback(mismatched));
  CHECK_FALSE(route->should_retain(mismatched));
  CHECK(route->should_discard(mismatched));
  route->invoke(mismatched);

  CHECK(route->has_callback(accepted));
  CHECK(route->should_retain(accepted));
  CHECK_FALSE(route->should_discard(accepted));
  route->invoke(accepted);
  CHECK_FALSE(route->has_callback(accepted));
}
