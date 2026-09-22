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
  const auto route = fixture.add(301, {.tools = frozen_tools(tools)});
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  const auto caller_thread = std::this_thread::get_id();
  CHECK(handler_thread == std::thread::id{});
  CHECK(fixture.pump.update({}).callbacks_delivered == 1);

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
  const auto route = fixture.add(302, {
                                          .tools = frozen_tools(tools),
                                          .callbacks =
                                              {
                                                  .on_tool_call =
                                                      [&](const scry::ToolCall& call) {
                                                        queued_when_observed =
                                                            fixture.commands->size();
                                                        observed_id = call.id;
                                                        observed_result = call.result;
                                                        observed_is_error =
                                                            call.is_error;
                                                      },
                                              },
                                      });
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  CHECK(fixture.pump.update({}).callbacks_delivered == 1);

  CHECK(queued_when_observed == 1);
  CHECK(observed_id == "call-1");
  CHECK(observed_result.text == R"({"ok":true})");
  CHECK_FALSE(observed_is_error);
  const auto result = pop_tool_result(fixture);
  REQUIRE(result);
  CHECK(result->result.text == R"({"ok":true})");
}

TEST_CASE("the observed ToolCall carries every field of the call it answers") {
  PumpFixture fixture;
  std::string handler_call_id;
  std::string handler_tool_name;
  std::string handler_arguments;
  const scry::detail::ToolSnapshot tools{
      registered_tool("forecast",
                      [&](const scry::ToolCallContext& context,
                          scry::Json arguments) -> scry::Result<scry::Json> {
                        handler_call_id = context.call_id;
                        handler_tool_name = context.tool_name;
                        handler_arguments = arguments.text;
                        return scry::Json{.text = R"({"ok":true})"};
                      })};
  std::string admitted_call_id;
  std::string admitted_arguments;
  std::optional<scry::ToolCall> observed;
  const auto route = fixture.add(
      312, {
               .tools = frozen_tools(tools),
               .callbacks = {
                   .on_tool_request = [&](const scry::ToolRequest& request)
                       -> std::optional<scry::ToolRejection> {
                     admitted_call_id = request.context.call_id;
                     admitted_arguments = request.arguments.text;
                     return std::nullopt;
                   },
                   .on_tool_call =
                       [&observed](const scry::ToolCall& call) { observed = call; },
               },
           });
  auto event = tool_event(route->id(), "forecast", "call-77");
  event.round = 3;
  event.index = 2;
  REQUIRE(fixture.events->push(event, 1024));

  CHECK(fixture.pump.update({}).callbacks_delivered == 1);

  // The admission hook and the handler borrow the live call, so both still see
  // it whole; the observation owns what the event no longer needs.
  CHECK(admitted_call_id == "call-77");
  CHECK(admitted_arguments == R"({"z":2,"a":1})");
  CHECK(handler_call_id == "call-77");
  CHECK(handler_tool_name == "forecast");
  CHECK(handler_arguments == R"({"z":2,"a":1})");
  REQUIRE(observed);
  CHECK(observed->turn_id == route->id());
  CHECK(observed->id == "call-77");
  CHECK(observed->name == "forecast");
  CHECK(observed->arguments.text == R"({"z":2,"a":1})");
  CHECK(observed->result.text == R"({"ok":true})");
  CHECK_FALSE(observed->is_error);
  CHECK(observed->round == 3);
  CHECK(observed->index == 2);
  const auto result = pop_tool_result(fixture);
  REQUIRE(result);
  CHECK(result->tool_call_id == "call-77");
  CHECK(result->result.text == R"({"ok":true})");
}

TEST_CASE("an unknown tool reaches the observer as an error result") {
  PumpFixture fixture;
  const scry::detail::ToolSnapshot tools{};
  scry::Json observed_result{};
  bool observed_is_error = false;
  std::size_t observer_calls = 0;
  const auto route = fixture.add(303, {
                                          .tools = frozen_tools(tools),
                                          .callbacks =
                                              {
                                                  .on_tool_call =
                                                      [&](const scry::ToolCall& call) {
                                                        ++observer_calls;
                                                        observed_result = call.result;
                                                        observed_is_error =
                                                            call.is_error;
                                                      },
                                              },
                                      });
  REQUIRE(fixture.events->push(tool_event(route->id(), "absent_tool"), 1024));

  CHECK(fixture.pump.update({}).callbacks_delivered == 1);

  CHECK(observer_calls == 1);
  CHECK(observed_is_error);
  CHECK(observed_result.text ==
        R"({"error":"unknown tool \"absent_tool\"; no tools are registered"})");
  const auto result = pop_tool_result(fixture);
  REQUIRE(result);
  CHECK(result->is_error);
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
  route = fixture.add(
      303, {
               .tools = frozen_tools(std::move(tools)),
               .callbacks =
                   {
                       .on_tool_call = [&observer_calls](
                                           const scry::ToolCall&) { ++observer_calls; },
                   },
           });
  REQUIRE(fixture.events->push(tool_event(route->id(), "first", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "second", "call-2"), 1024));

  const auto stats = fixture.pump.update({});

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
      fixture.add(306, {.tools = frozen_tools(tools), .max_tool_result_bytes = 2});
  REQUIRE(fixture.events->push(tool_event(route->id(), "first", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "second", "call-2"), 1024));

  const auto stats = fixture.pump.update({});

  CHECK(stats.events_remaining == 0);
  CHECK(first_calls == 1);
  CHECK(second_calls == 0);
  CHECK(fixture.commands->size() == 1);
  const auto result = pop_tool_result(fixture);
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
  const auto route = fixture.add(307, {.tools = frozen_tools(tools)});
  constexpr std::size_t two_results_minus_one = 17;
  REQUIRE(fixture.events->push(
      tool_event(route->id(), "first", "call-1", two_results_minus_one), 1024));
  REQUIRE(fixture.events->push(
      tool_event(route->id(), "second", "call-2", two_results_minus_one), 1024));
  REQUIRE(fixture.events->push(
      tool_event(route->id(), "third", "call-3", two_results_minus_one), 1024));

  const auto stats = fixture.pump.update({});

  CHECK(stats.events_remaining == 0);
  CHECK(calls == std::array<std::size_t, 3>{1, 1, 0});
  CHECK(fixture.commands->size() == 2);
  CHECK(pop_tool_result(fixture).has_value());
  CHECK_FALSE(pop_tool_result(fixture).has_value());
}

TEST_CASE("detached routes continue dispatching tool calls") {
  PumpFixture fixture;
  std::size_t handler_calls = 0;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = R"({"ok":true})"};
      })};
  const auto route = fixture.add(304, {.tools = frozen_tools(tools)});
  route->detach();
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));

  CHECK(fixture.pump.update({}).callbacks_delivered == 1);

  CHECK(handler_calls == 1);
  CHECK(pop_tool_result(fixture));
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
  const auto route = fixture.add(
      305, {
               .tools = frozen_tools(tools),
               .callbacks = {
                   .on_tool_call =
                       [&observer_calls](const scry::ToolCall&) { ++observer_calls; },
                   .on_finished =
                       [&completed](scry::Result<scry::Completion> done) {
                         completed = done.has_value();
                       },
               },
           });
  REQUIRE(fixture.events->push(tool_event(route->id()), 1024));
  CHECK(fixture.pump.update({.max_callbacks = 0}).events_remaining == 1);
  REQUIRE(fixture.events->push(completion_event(route->id()), 1024));

  const auto terminal = fixture.pump.update({});

  CHECK(terminal.callbacks_delivered == 1);
  CHECK(terminal.events_remaining == 0);
  CHECK(completed);
  CHECK(route->terminal());
  CHECK(handler_calls == 0);
  CHECK(observer_calls == 0);
  CHECK(fixture.commands->size() == 0);
  CHECK(fixture.pump.live_route_count() == 0);
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
  const auto route = fixture.add(
      310, {
               .tools = frozen_tools(tools),
               .callbacks = {
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
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(fixture.pump.update({}).events_remaining == 0);

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
    const auto posted = pop_tool_result(fixture);
    REQUIRE(posted);
    CHECK(posted->is_error == expected_error);
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
  const auto route = fixture.add(
      311, {
               .tools = frozen_tools(tools),
               .max_tool_calls = 1,
               .callbacks = {
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
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(fixture.pump.update({}).events_remaining == 0);

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
  const auto route = fixture.add(
      312, {
               .tools = frozen_tools(tools),
               .max_tool_calls = 1,
               .callbacks = {
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
  REQUIRE(fixture.events->push(tool_event(route->id(), "absent_tool", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(fixture.pump.update({}).events_remaining == 0);

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
      fixture.add(313, {
                           .tools = frozen_tools(tools),
                           .callbacks = {
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
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  CHECK(fixture.pump.update({}).events_remaining == 0);

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
  route = fixture.add(
      314,
      {
          .tools = frozen_tools(tools),
          .callbacks = {
              .on_tool_request =
                  [&](const scry::ToolRequest&) -> std::optional<scry::ToolRejection> {
                disconnected = route->disconnect();
                return scry::ToolRejection{.model_message = "no more tools"};
              },
              .on_tool_call =
                  [&observer_calls](const scry::ToolCall&) { ++observer_calls; },
          },
      });
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));

  CHECK(fixture.pump.update({}).events_remaining == 0);

  CHECK(disconnected);
  CHECK(observer_calls == 0);
  // Dropping the callbacks stops delivery, not the loop: the refusal still
  // reaches the worker so the model gets an answer for the call it made.
  CHECK(fixture.commands->size() == 1);
  const auto posted = pop_tool_result(fixture);
  REQUIRE(posted);
  CHECK(posted->is_error);
  CHECK(posted->result.text == R"({"error":"no more tools"})");
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
  route = fixture.add(
      315,
      {
          .tools = frozen_tools(tools),
          .callbacks = {
              .on_tool_request =
                  [&](const scry::ToolRequest&) -> std::optional<scry::ToolRejection> {
                cancellation_requested = route->cancel();
                return std::nullopt;
              },
              .on_tool_call =
                  [&observer_calls](const scry::ToolCall&) { ++observer_calls; },
          },
      });
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-1"), 1024));

  CHECK(fixture.pump.update({}).events_remaining == 0);

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
