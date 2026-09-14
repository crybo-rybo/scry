#include "runtime_test_support.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
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
  const auto route = fixture.route(301, {.tools = frozen_tools(tools)});
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
  std::size_t queued_when_observed = 0;
  std::string observed_id;
  scry::Json observed_result{};
  bool observed_is_error = true;
  const auto route =
      fixture.route(302, {
                             .tools = frozen_tools(tools),
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_tool_call =
                                         [&](const scry::ToolCall& call) {
                                           queued_when_observed =
                                               fixture.commands->size();
                                           observed_id = call.id;
                                           observed_result = call.result;
                                           observed_is_error = call.is_error;
                                         },
                                 },
                         });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  CHECK(queued_when_observed == 1);
  CHECK(observed_id == "call-1");
  CHECK(observed_result.text == R"({"ok":true})");
  CHECK_FALSE(observed_is_error);
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  const auto* result = std::get_if<scry::detail::ToolResultCommand>(&*command);
  REQUIRE(result);
  REQUIRE(result->result);
  CHECK(result->result->result.text == R"({"ok":true})");
}

TEST_CASE("an unknown tool reaches the observer as an error result") {
  PumpFixture fixture;
  const scry::detail::ToolSnapshot tools{};
  scry::Json observed_result{};
  bool observed_is_error = false;
  std::size_t observer_calls = 0;
  const auto route =
      fixture.route(303, {
                             .tools = frozen_tools(tools),
                             .callbacks =
                                 scry::TurnCallbacks{
                                     .on_tool_call =
                                         [&](const scry::ToolCall& call) {
                                           ++observer_calls;
                                           observed_result = call.result;
                                           observed_is_error = call.is_error;
                                         },
                                 },
                         });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "absent_tool"), 1024));

  CHECK(pump.update({}).callbacks_delivered == 1);

  CHECK(observer_calls == 1);
  CHECK(observed_is_error);
  CHECK(observed_result.text ==
        R"({"error":"unknown tool \"absent_tool\"; no tools are registered"})");
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  const auto* result = std::get_if<scry::detail::ToolResultCommand>(&*command);
  REQUIRE(result);
  REQUIRE(result->result);
  CHECK(result->result->is_error);
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
  route = fixture.route(
      303, {
               .tools = frozen_tools(std::move(tools)),
               .callbacks =
                   scry::TurnCallbacks{
                       .on_tool_call = [&observer_calls](
                                           const scry::ToolCall&) { ++observer_calls; },
                   },
           });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
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
  const auto route =
      fixture.route(306, {.tools = frozen_tools(tools), .max_tool_result_bytes = 2});
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
  const auto route = fixture.route(307, {.tools = frozen_tools(tools)});
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
  const auto route = fixture.route(304, {.tools = frozen_tools(tools)});
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
  const auto route = fixture.route(
      305, {
               .tools = frozen_tools(tools),
               .callbacks = scry::TurnCallbacks{
                   .on_tool_call =
                       [&observer_calls](const scry::ToolCall&) { ++observer_calls; },
                   .on_finished =
                       [&completed](scry::Result<scry::Completion> done) {
                         completed = done.has_value();
                       },
               },
           });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
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

TEST_CASE("an admission hook refuses one call and lets the next through") {
  PumpFixture fixture;
  std::size_t handler_calls = 0;
  std::size_t admissions = 0;
  std::string second_request_call_id;
  std::string second_request_tool_name;
  std::string second_request_arguments;
  std::vector<bool> observed_errors;
  std::vector<std::string> observed_results;
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [&](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route = fixture.route(
      310, {
               .tools = frozen_tools(tools),
               .callbacks = scry::TurnCallbacks{
                   .on_tool_request = [&](const scry::ToolRequest& request)
                       -> std::optional<scry::ToolRejection> {
                     if (++admissions == 1) {
                       return std::nullopt;
                     }
                     second_request_call_id = std::string{request.context.call_id};
                     second_request_tool_name = std::string{request.context.tool_name};
                     second_request_arguments = request.arguments.text;
                     return scry::ToolRejection{.model_message = "one call is enough"};
                   },
                   .on_tool_call =
                       [&](const scry::ToolCall& call) {
                         observed_errors.push_back(call.is_error);
                         observed_results.push_back(call.result.text);
                       },
               },
           });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(pump.update({}).events_remaining == 0);

  CHECK(admissions == 2);
  CHECK(handler_calls == 1);
  CHECK(second_request_call_id == "call-2");
  CHECK(second_request_tool_name == "forecast");
  CHECK(second_request_arguments == R"({"z":2,"a":1})");
  CHECK(observed_errors == std::vector<bool>{false, true});
  REQUIRE(observed_results.size() == 2);
  CHECK(observed_results[0] == R"({"ok":true})");
  CHECK(observed_results[1] == R"({"error":"one call is enough"})");
  // The refusal is a result like any other: the worker gets both, so the model
  // sees an answer for every call it made.
  CHECK(fixture.commands->size() == 2);
  for (const auto expected_error : {false, true}) {
    auto command = fixture.commands->try_pop();
    REQUIRE(command);
    const auto* posted = std::get_if<scry::detail::ToolResultCommand>(&*command);
    REQUIRE(posted);
    REQUIRE(posted->result);
    CHECK(posted->result->is_error == expected_error);
  }
}

TEST_CASE("the per-turn call limit refuses calls past it without failing the turn") {
  PumpFixture fixture;
  std::size_t handler_calls = 0;
  std::size_t admissions = 0;
  std::vector<std::string> observed_results;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route = fixture.route(
      311, {
               .tools = frozen_tools(tools),
               .max_tool_calls = 1,
               .callbacks = scry::TurnCallbacks{
                   .on_tool_request = [&admissions](const scry::ToolRequest&)
                       -> std::optional<scry::ToolRejection> {
                     ++admissions;
                     return std::nullopt;
                   },
                   .on_tool_call =
                       [&observed_results](const scry::ToolCall& call) {
                         observed_results.push_back(call.result.text);
                       },
               },
           });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(pump.update({}).events_remaining == 0);

  CHECK(handler_calls == 1);
  // The limit is checked before the hook, so the refused call never reaches it.
  CHECK(admissions == 1);
  REQUIRE(observed_results.size() == 2);
  CHECK(observed_results[0] == R"({"ok":true})");
  CHECK(
      observed_results[1] ==
      R"({"error":"tool call limit for this turn reached; respond without calling tools"})");
  CHECK(fixture.commands->size() == 2);
}

TEST_CASE("an unknown tool spends the call limit without consulting the hook") {
  PumpFixture fixture;
  std::size_t admissions = 0;
  std::vector<std::string> observed_results;
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route = fixture.route(
      312, {
               .tools = frozen_tools(tools),
               .max_tool_calls = 1,
               .callbacks = scry::TurnCallbacks{
                   .on_tool_request = [&admissions](const scry::ToolRequest&)
                       -> std::optional<scry::ToolRejection> {
                     ++admissions;
                     return std::nullopt;
                   },
                   .on_tool_call =
                       [&observed_results](const scry::ToolCall& call) {
                         observed_results.push_back(call.result.text);
                       },
               },
           });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "absent_tool", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(pump.update({}).events_remaining == 0);

  CHECK(admissions == 0);
  REQUIRE(observed_results.size() == 2);
  CHECK(observed_results[0] ==
        R"({"error":"unknown tool \"absent_tool\"; registered tools: forecast"})");
  CHECK(
      observed_results[1] ==
      R"({"error":"tool call limit for this turn reached; respond without calling tools"})");
}

TEST_CASE("a throwing admission hook refuses the call and the turn continues") {
  PumpFixture fixture;
  std::size_t handler_calls = 0;
  std::vector<std::string> observed_results;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route =
      fixture.route(313, {
                             .tools = frozen_tools(tools),
                             .callbacks = scry::TurnCallbacks{
                                 .on_tool_request = [](const scry::ToolRequest&)
                                     -> std::optional<scry::ToolRejection> {
                                   throw std::runtime_error{"policy exploded"};
                                 },
                                 .on_tool_call =
                                     [&observed_results](const scry::ToolCall& call) {
                                       observed_results.push_back(call.result.text);
                                     },
                             },
                         });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(pump.update({}).events_remaining == 0);

  CHECK(handler_calls == 0);
  CHECK(observed_results ==
        std::vector<std::string>{R"({"error":"tool handler threw an exception"})",
                                 R"({"error":"tool handler threw an exception"})"});
  CHECK(fixture.commands->size() == 2);
}

TEST_CASE("an admission hook that disconnects suppresses its own observation") {
  PumpFixture fixture;
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t observer_calls = 0;
  bool disconnected = false;
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast", [](scry::Json) -> scry::Result<scry::Json> {
        return scry::Json{.text = R"({"ok":true})"};
      })};
  route = fixture.route(
      314,
      {
          .tools = frozen_tools(tools),
          .callbacks = scry::TurnCallbacks{
              .on_tool_request =
                  [&](const scry::ToolRequest&) -> std::optional<scry::ToolRejection> {
                disconnected = route->disconnect();
                return scry::ToolRejection{.model_message = "no more tools"};
              },
              .on_tool_call =
                  [&observer_calls](const scry::ToolCall&) { ++observer_calls; },
          },
      });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));

  CHECK(pump.update({}).events_remaining == 0);

  CHECK(disconnected);
  CHECK(observer_calls == 0);
  // Dropping the callbacks stops delivery, not the loop: the refusal still
  // reaches the worker so the model gets an answer for the call it made.
  CHECK(fixture.commands->size() == 1);
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  const auto* posted = std::get_if<scry::detail::ToolResultCommand>(&*command);
  REQUIRE(posted);
  REQUIRE(posted->result);
  CHECK(posted->result->is_error);
  CHECK(posted->result->result.text == R"({"error":"no more tools"})");
}

TEST_CASE("an admission hook that cancels stops the call it admitted") {
  PumpFixture fixture;
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t handler_calls = 0;
  std::size_t observer_calls = 0;
  bool cancellation_requested = false;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = R"({"ok":true})"};
      })};
  route = fixture.route(
      315,
      {
          .tools = frozen_tools(tools),
          .callbacks = scry::TurnCallbacks{
              .on_tool_request =
                  [&](const scry::ToolRequest&) -> std::optional<scry::ToolRejection> {
                cancellation_requested = route->cancel();
                return std::nullopt;
              },
              .on_tool_call =
                  [&observer_calls](const scry::ToolCall&) { ++observer_calls; },
          },
      });
  scry::detail::PumpState pump{fixture.events};
  pump.add_route(route);
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));

  CHECK(pump.update({}).events_remaining == 0);

  CHECK(cancellation_requested);
  // Admitting a call the host then cancelled is not permission to run it: the
  // handler's side effects would outlive the transcript the rollback discards.
  CHECK(handler_calls == 0);
  CHECK(observer_calls == 0);
  // Only the cancellation reaches the worker; no result is posted for the call.
  CHECK(fixture.commands->size() == 1);
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::CancelTurnCommand>(*command));
}
