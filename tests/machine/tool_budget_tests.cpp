#include "machine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>

using namespace std::chrono_literals;
using namespace scry::detail::machine_test;

namespace {

[[nodiscard]] scry::detail::ToolResultReady
result(std::string id, std::string json,
       const scry::detail::MachineTimePoint observed_at = at(1ms)) {
  return {
      .result =
          {
              .tool_call_id = std::move(id),
              .result = scry::Json{.text = std::move(json)},
          },
      .observed_at = observed_at,
  };
}

[[nodiscard]] scry::detail::ModelResponse
final_response(std::string text = "done", const std::uint64_t input_tokens = 7,
               const std::uint64_t output_tokens = 11) {
  return {
      .content = {scry::detail::TextBlock{.text = std::move(text)}},
      .finish_reason = scry::FinishReason::completed,
      .usage = {.input_tokens = input_tokens, .output_tokens = output_tokens},
      .provider_request_id = "final-request",
  };
}

} // namespace

TEST_CASE("usage overflow is rejected instead of wrapping") {
  const auto overflow_after = [](scry::Usage first_usage, scry::Usage final_usage) {
    auto machine = make_machine();
    begin(machine);
    auto response = tool_response();
    response.usage = first_usage;
    auto published =
        machine.apply(scry::detail::ModelCompleted{.response = std::move(response)});
    static_cast<void>(only_command<scry::detail::PublishToolCall>(published));
    auto issue = machine.apply(result("call-1", "{}", at(1ms)));
    static_cast<void>(only_command<scry::detail::IssueModelRequest>(issue));

    auto final =
        final_response("done", final_usage.input_tokens, final_usage.output_tokens);
    const auto overflow =
        machine.apply(scry::detail::ModelCompleted{.response = std::move(final)});
    CHECK(only_command<scry::detail::PublishError>(overflow).error.category ==
          scry::ErrorCategory::protocol);
  };

  SECTION("input usage") {
    overflow_after({.input_tokens = std::numeric_limits<std::uint64_t>::max()},
                   {.input_tokens = 1});
  }

  SECTION("output usage") {
    overflow_after({.output_tokens = std::numeric_limits<std::uint64_t>::max()},
                   {.output_tokens = 1});
  }
}

TEST_CASE("tool exchange budget rejects an assistant round before dispatch") {
  auto response = tool_response();
  const scry::detail::Message assistant{
      .role = scry::detail::Role::assistant,
      .content = response.content,
  };
  auto tools = tool_policy();
  tools.max_exchange_bytes = scry::detail::message_payload_bytes(assistant) - 1;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto rejected =
      machine.apply(scry::detail::ModelCompleted{.response = std::move(response)});

  const auto& error = only_command<scry::detail::PublishError>(rejected).error;
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.provider_request_id == "tool-request");
  CHECK(machine.terminal_kind() == scry::detail::MachineTerminalKind::failed);
}

TEST_CASE("tool exchange budget is exact across every ordered result") {
  auto response = tool_response({
      tool_call("call-a", "first"),
      tool_call("call-b", "second"),
  });
  const scry::detail::Message assistant{
      .role = scry::detail::Role::assistant,
      .content = response.content,
  };
  const scry::detail::ToolResultBlock first_result{
      .tool_call_id = "call-a",
      .result = scry::Json{.text = "{}"},
  };
  const scry::detail::ToolResultBlock second_result{
      .tool_call_id = "call-b",
      .result = scry::Json{.text = "{}"},
  };
  auto tools = tool_policy();
  tools.max_exchange_bytes = scry::detail::message_payload_bytes(assistant) +
                             scry::detail::content_payload_bytes(first_result) +
                             scry::detail::content_payload_bytes(second_result);
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto calls =
      machine.apply(scry::detail::ModelCompleted{.response = std::move(response)});
  REQUIRE(calls.commands.size() == 2);
  CHECK(std::get<scry::detail::PublishToolCall>(calls.commands.front())
            .remaining_exchange_bytes ==
        scry::detail::content_payload_bytes(first_result) +
            scry::detail::content_payload_bytes(second_result));
  CHECK(machine.apply(result("call-a", "{}", at(1ms))).commands.empty());
  const auto next = machine.apply(result("call-b", "{}", at(2ms)));
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(next));
}

TEST_CASE("cumulative tool results cannot cross the exchange budget") {
  auto response = tool_response({
      tool_call("call-a", "first"),
      tool_call("call-b", "second"),
  });
  const scry::detail::Message assistant{
      .role = scry::detail::Role::assistant,
      .content = response.content,
  };
  const scry::detail::ToolResultBlock result_block{
      .tool_call_id = "call-a",
      .result = scry::Json{.text = "{}"},
  };
  auto tools = tool_policy();
  tools.max_exchange_bytes = scry::detail::message_payload_bytes(assistant) +
                             (2 * scry::detail::content_payload_bytes(result_block)) -
                             1;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);
  static_cast<void>(
      machine.apply(scry::detail::ModelCompleted{.response = std::move(response)}));
  CHECK(machine.apply(result("call-a", "{}", at(1ms))).commands.empty());

  const auto rejected = machine.apply(result("call-b", "{}", at(2ms)));

  const auto& error = only_command<scry::detail::PublishError>(rejected).error;
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.provider_request_id == "tool-request");
}

TEST_CASE("final completion must fit the remaining exchange budget") {
  auto tools = tool_policy();
  tools.max_exchange_bytes = 3;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto rejected =
      machine.apply(scry::detail::ModelCompleted{.response = final_response("done")});

  const auto& error = only_command<scry::detail::PublishError>(rejected).error;
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.provider_request_id == "final-request");
}

TEST_CASE("framework failures preserve their own provider correlation") {
  auto machine = make_machine();
  enter_awaiting_tool(machine);
  auto dispatch_error = error(scry::ErrorCategory::resource_limit, "dispatch failed");
  dispatch_error.provider_request_id = "dispatch-request";

  const auto failed = machine.apply(scry::detail::ToolExecutionFailed{
      .error = std::move(dispatch_error),
  });

  CHECK(only_command<scry::detail::PublishError>(failed).error.provider_request_id ==
        "dispatch-request");
}
