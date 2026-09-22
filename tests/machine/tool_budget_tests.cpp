#include "machine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>
#include <variant>

using namespace std::chrono_literals;
using namespace scry::detail::machine_test;

TEST_CASE("usage overflow is rejected instead of wrapping") {
  const auto overflow_after = [](scry::Usage first_usage, scry::Usage final_usage) {
    auto machine = make_machine();
    begin(machine);
    auto response = tool_response();
    response.usage = first_usage;
    auto published = machine.apply(ModelCompleted{.response = std::move(response)});
    only_command<PublishToolCall>(published);
    auto issue = machine.apply(result("call-1", "{}", at(1ms)));
    only_command<IssueModelRequest>(issue);

    auto final =
        final_response("done", final_usage.input_tokens, final_usage.output_tokens);
    const auto overflow = machine.apply(ModelCompleted{.response = std::move(final)});
    CHECK(only_command<PublishError>(overflow).error.category ==
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
  const Message assistant{
      .role = Role::assistant,
      .content = response.content,
  };
  auto tools = tool_policy();
  tools.max_exchange_bytes = message_payload_bytes(assistant) - 1;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto rejected = machine.apply(ModelCompleted{.response = std::move(response)});

  const auto& error = only_command<PublishError>(rejected).error;
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.provider_request_id == "tool-request");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("tool exchange budget is exact across every ordered result") {
  auto response = tool_response({
      tool_call("call-a", "first"),
      tool_call("call-b", "second"),
  });
  const Message assistant{
      .role = Role::assistant,
      .content = response.content,
  };
  const ToolResultBlock first_result{
      .tool_call_id = "call-a",
      .result = scry::Json{.text = "{}"},
  };
  const ToolResultBlock second_result{
      .tool_call_id = "call-b",
      .result = scry::Json{.text = "{}"},
  };
  auto tools = tool_policy();
  tools.max_exchange_bytes = message_payload_bytes(assistant) +
                             content_payload_bytes(first_result) +
                             content_payload_bytes(second_result);
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto calls = machine.apply(ModelCompleted{.response = std::move(response)});
  REQUIRE(calls.commands.size() == 2);
  CHECK(machine.apply(result("call-a", "{}", at(1ms))).commands.empty());
  const auto next = machine.apply(result("call-b", "{}", at(2ms)));
  only_command<IssueModelRequest>(next);
}

TEST_CASE("cumulative tool results cannot cross the exchange budget") {
  auto response = tool_response({
      tool_call("call-a", "first"),
      tool_call("call-b", "second"),
  });
  const Message assistant{
      .role = Role::assistant,
      .content = response.content,
  };
  const ToolResultBlock result_block{
      .tool_call_id = "call-a",
      .result = scry::Json{.text = "{}"},
  };
  auto tools = tool_policy();
  tools.max_exchange_bytes =
      message_payload_bytes(assistant) + (2 * content_payload_bytes(result_block)) - 1;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);
  static_cast<void>(machine.apply(ModelCompleted{.response = std::move(response)}));
  CHECK(machine.apply(result("call-a", "{}", at(1ms))).commands.empty());

  const auto rejected = machine.apply(result("call-b", "{}", at(2ms)));

  const auto& error = only_command<PublishError>(rejected).error;
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.provider_request_id == "tool-request");
}

TEST_CASE("final completion must fit the remaining exchange budget") {
  auto tools = tool_policy();
  tools.max_exchange_bytes = 3;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto rejected =
      machine.apply(ModelCompleted{.response = final_response("done")});

  const auto& error = only_command<PublishError>(rejected).error;
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.provider_request_id == "final-request");
}

TEST_CASE("framework failures preserve their own provider correlation") {
  auto machine = make_machine();
  enter_awaiting_tool(machine);
  auto dispatch_error = error(scry::ErrorCategory::resource_limit, "dispatch failed");
  dispatch_error.provider_request_id = "dispatch-request";

  const auto failed = machine.apply(ToolExecutionFailed{
      .error = std::move(dispatch_error),
  });

  CHECK(only_command<PublishError>(failed).error.provider_request_id ==
        "dispatch-request");
}

namespace {

// Runs exactly one tool round on a machine whose cap is one, leaving it awaiting
// the response that will ask for a second round it cannot have.
void run_one_round(TurnMachine& machine) {
  begin(machine);
  const auto first = machine.apply(ModelCompleted{.response = tool_response()});
  only_command<PublishToolCall>(first);
  const auto issue = machine.apply(result("call-1", R"({"ok":true})", at(1ms)));
  only_command<IssueModelRequest>(issue);
}

[[nodiscard]] ToolLoopPolicy soft_stop_policy() {
  auto tools = tool_policy();
  tools.max_rounds = 1;
  tools.limit_policy = scry::ToolRoundLimitPolicy::complete;
  return tools;
}

} // namespace

TEST_CASE(
    "soft-stop round limit commits the final text and reports its dropped calls") {
  auto machine = make_machine(retry_policy(), soft_stop_policy());
  run_one_round(machine);

  const auto stopped = machine.apply(ModelCompleted{
      .response = tool_response({
          TextBlock{.text = "wrapping up"},
          tool_call("call-2", "again"),
      }),
  });

  const auto& commit = only_command<CommitCompletion>(stopped);
  CHECK(commit.finish_reason == scry::FinishReason::tool_round_limit);
  CHECK(commit.tool_round_count == 1);
  CHECK(commit.tool_call_count == 1);
  CHECK(commit.provider_request_id == "tool-request");
  // Two responses of the same shape, both counted, the stopping one included.
  CHECK(commit.usage.input_tokens == 4);
  CHECK(commit.usage.output_tokens == 6);
  REQUIRE(commit.unexecuted_tool_calls.size() == 1);
  CHECK(commit.unexecuted_tool_calls.front().id == "call-2");
  CHECK(commit.unexecuted_tool_calls.front().name == "again");
  CHECK(commit.unexecuted_tool_calls.front().arguments.text == R"({"x":1})");
  // The committed assistant message keeps the text and none of the calls.
  REQUIRE(commit.transcript.size() == 4);
  const auto& last = commit.transcript.back();
  CHECK(last.role == Role::assistant);
  REQUIRE(last.content.size() == 1);
  CHECK(std::get<TextBlock>(last.content.front()).text == "wrapping up");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("a calls-only response at the soft-stop limit commits no empty message") {
  auto machine = make_machine(retry_policy(), soft_stop_policy());
  run_one_round(machine);

  const auto stopped = machine.apply(ModelCompleted{
      .response = tool_response({tool_call("call-2", "again")}),
  });

  const auto& commit = only_command<CommitCompletion>(stopped);
  CHECK(commit.finish_reason == scry::FinishReason::tool_round_limit);
  REQUIRE(commit.unexecuted_tool_calls.size() == 1);
  // Every committed message holds at least one block, so the transcript stops at
  // the previous round's results rather than gaining an empty assistant message.
  REQUIRE(commit.transcript.size() == 3);
  const auto& last = commit.transcript.back();
  CHECK(last.role == Role::user);
  REQUIRE(last.content.size() == 1);
  CHECK(std::get<ToolResultBlock>(last.content.front()).tool_call_id == "call-1");
}

TEST_CASE("dropped calls must fit the remaining exchange budget") {
  const Message round_assistant{
      .role = Role::assistant,
      .content = tool_response().content,
  };
  const ToolResultBlock round_result{
      .tool_call_id = "call-1",
      .result = scry::Json{.text = R"({"ok":true})"},
  };
  const auto dropped = tool_call("call-2", "again");
  // Room for the round that runs, one byte short of the calls it may not run.
  auto tools = soft_stop_policy();
  tools.max_exchange_bytes = message_payload_bytes(round_assistant) +
                             content_payload_bytes(round_result) +
                             content_payload_bytes(dropped) - 1;
  auto machine = make_machine(retry_policy(), tools);
  run_one_round(machine);

  const auto rejected = machine.apply(ModelCompleted{
      .response = tool_response({
          TextBlock{.text = "wrapping up"},
          dropped,
      }),
  });

  const auto& error = only_command<PublishError>(rejected).error;
  CHECK(error.category == scry::ErrorCategory::resource_limit);
  CHECK(error.provider_request_id == "tool-request");
  CHECK(machine.phase() == MachinePhase::terminal);
}
