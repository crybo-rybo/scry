#include "machine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::detail::machine_test;

namespace {

[[nodiscard]] scry::detail::ToolResultReady
result(std::string id, std::string json,
       const scry::detail::MachineTimePoint observed_at = at(1ms),
       const bool is_error = false) {
  return {
      .result =
          {
              .tool_call_id = std::move(id),
              .result = scry::Json{.text = std::move(json)},
              .is_error = is_error,
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

[[nodiscard]] const scry::detail::Message&
message_at(const std::vector<scry::detail::Message>& messages, const std::size_t index,
           const scry::detail::Role role) {
  REQUIRE(index < messages.size());
  CHECK(messages[index].role == role);
  return messages[index];
}

void check_rejected_response(scry::detail::ModelResponse response,
                             const scry::ErrorCategory category) {
  const auto provider_request_id = response.provider_request_id;
  auto machine = make_machine();
  begin(machine);
  const auto transition =
      machine.apply(scry::detail::ModelCompleted{.response = std::move(response)});
  const auto& error = only_command<scry::detail::PublishError>(transition).error;
  CHECK(error.category == category);
  CHECK(error.turn_id == turn_id);
  CHECK(error.attempt == 1);
  CHECK(error.provider_request_id == provider_request_id);
}

} // namespace

TEST_CASE("tool response publishes every call once in provider order") {
  auto machine = make_machine();
  begin(machine);

  auto response = tool_response({
      scry::detail::TextBlock{.text = "I will look twice."},
      tool_call("call-a", "first", R"({"value":1})"),
      scry::detail::TextBlock{.text = "Then compare."},
      tool_call("call-b", "second", R"({"value":2})"),
  });
  const auto transition =
      machine.apply(scry::detail::ModelCompleted{.response = std::move(response)});

  REQUIRE(transition.status == scry::detail::TransitionStatus::applied);
  REQUIRE(transition.commands.size() == 2);
  const auto& first = std::get<scry::detail::PublishToolCall>(transition.commands[0]);
  const auto& second = std::get<scry::detail::PublishToolCall>(transition.commands[1]);
  CHECK(first.turn_id == turn_id);
  CHECK(first.call.id == "call-a");
  CHECK(first.call.name == "first");
  CHECK(second.call.id == "call-b");
  CHECK(second.call.name == "second");
  CHECK(machine.phase() == scry::detail::MachinePhase::awaiting_tool);
}

TEST_CASE("out-of-order tool results produce one ordered result message") {
  auto machine = make_machine();
  begin(machine);
  const auto published = machine.apply(scry::detail::ModelCompleted{
      .response = tool_response({
          tool_call("call-a", "first"),
          tool_call("call-b", "second"),
      }),
  });
  REQUIRE(published.commands.size() == 2);

  const auto second = machine.apply(result("call-b", R"({"order":2})", at(2ms), true));
  CHECK(second.status == scry::detail::TransitionStatus::applied);
  CHECK(second.commands.empty());
  CHECK(machine.phase() == scry::detail::MachinePhase::awaiting_tool);

  const auto first = machine.apply(result("call-a", R"({"order":1})", at(3ms)));
  const auto& issue = only_command<scry::detail::IssueModelRequest>(first);
  REQUIRE(issue.request->messages.size() == 3);
  const auto& assistant =
      message_at(issue.request->messages, 1, scry::detail::Role::assistant);
  REQUIRE(assistant.content.size() == 2);
  const auto& results =
      message_at(issue.request->messages, 2, scry::detail::Role::user);
  REQUIRE(results.content.size() == 2);
  const auto& first_result =
      std::get<scry::detail::ToolResultBlock>(results.content[0]);
  const auto& second_result =
      std::get<scry::detail::ToolResultBlock>(results.content[1]);
  CHECK(first_result.tool_call_id == "call-a");
  CHECK(first_result.result.text == R"({"order":1})");
  CHECK_FALSE(first_result.is_error);
  CHECK(second_result.tool_call_id == "call-b");
  CHECK(second_result.result.text == R"({"order":2})");
  CHECK(second_result.is_error);
  CHECK(issue.attempt == 2);
}

TEST_CASE("multi-round completion carries the transactional exchange and totals") {
  auto machine = make_machine();
  begin(machine);

  auto round_one =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  static_cast<void>(only_command<scry::detail::PublishToolCall>(round_one));
  auto next = machine.apply(result("call-1", R"({"one":1})", at(10ms)));
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(next));

  auto second_response = tool_response({tool_call("call-2", "second")});
  second_response.usage = {.input_tokens = 5, .output_tokens = 7};
  auto round_two = machine.apply(
      scry::detail::ModelCompleted{.response = std::move(second_response)});
  static_cast<void>(only_command<scry::detail::PublishToolCall>(round_two));
  next = machine.apply(result("call-2", R"({"two":2})", at(20ms)));
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(next));

  const auto completed = machine.apply(
      scry::detail::ModelCompleted{.response = final_response("finished", 11, 13)});
  const auto& commit = only_command<scry::detail::CommitCompletion>(completed);

  CHECK(commit.attempt_count == 3);
  CHECK(commit.usage.input_tokens == 18);
  CHECK(commit.usage.output_tokens == 23);
  CHECK(commit.finish_reason == scry::FinishReason::completed);
  CHECK(commit.provider_request_id == "final-request");
  REQUIRE(commit.exchange.size() == 5);
  static_cast<void>(message_at(commit.exchange, 0, scry::detail::Role::assistant));
  static_cast<void>(message_at(commit.exchange, 1, scry::detail::Role::user));
  static_cast<void>(message_at(commit.exchange, 2, scry::detail::Role::assistant));
  static_cast<void>(message_at(commit.exchange, 3, scry::detail::Role::user));
  const auto& final = message_at(commit.exchange, 4, scry::detail::Role::assistant);
  REQUIRE(final.content.size() == 1);
  CHECK(std::get<scry::detail::TextBlock>(final.content.front()).text == "finished");
}

TEST_CASE("tool-round cap fails before publishing any call from excess round") {
  auto tools = tool_policy();
  tools.max_rounds = 1;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  auto first = machine.apply(scry::detail::ModelCompleted{
      .response = tool_response({
          tool_call("call-1", "first"),
          tool_call("call-1b", "second"),
      }),
  });
  REQUIRE(first.commands.size() == 2);
  auto partial = machine.apply(result("call-1", "{}", at(1ms)));
  CHECK(partial.commands.empty());
  auto issue = machine.apply(result("call-1b", "{}", at(2ms)));
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(issue));

  const auto excess = machine.apply(scry::detail::ModelCompleted{
      .response = tool_response({tool_call("call-2", "again")}),
  });
  const auto& error = only_command<scry::detail::PublishError>(excess).error;
  CHECK(error.category == scry::ErrorCategory::max_tool_rounds);
  CHECK(error.turn_id == turn_id);
  CHECK(error.attempt == 2);
  CHECK(error.provider_request_id == "tool-request");
  CHECK(machine.terminal_kind() == scry::detail::MachineTerminalKind::failed);
}

TEST_CASE("zero tool-round cap rejects the first call batch") {
  auto tools = tool_policy();
  tools.max_rounds = 0;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto transition =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  CHECK(only_command<scry::detail::PublishError>(transition).error.category ==
        scry::ErrorCategory::max_tool_rounds);
}

TEST_CASE("model completion validates finish and content consistency") {
  using scry::ErrorCategory;

  SECTION("tool-use finish requires at least one call") {
    check_rejected_response(tool_response({scry::detail::TextBlock{.text = "none"}}),
                            ErrorCategory::protocol);
  }

  SECTION("calls require a tool-use finish") {
    auto response = tool_response();
    response.finish_reason = scry::FinishReason::completed;
    check_rejected_response(std::move(response), ErrorCategory::protocol);
  }

  SECTION("assistant responses cannot contain tool results") {
    auto response = final_response();
    response.content = {scry::detail::ToolResultBlock{
        .tool_call_id = "call-1",
        .result = scry::Json{.text = "{}"},
    }};
    check_rejected_response(std::move(response), ErrorCategory::protocol);
  }
}

TEST_CASE("model completion validates tool call identities and arguments") {
  using scry::ErrorCategory;

  SECTION("call ID must be non-empty") {
    check_rejected_response(tool_response({tool_call("", "lookup")}),
                            ErrorCategory::protocol);
  }

  SECTION("call name must be non-empty") {
    check_rejected_response(tool_response({tool_call("call-1", "")}),
                            ErrorCategory::protocol);
  }

  SECTION("IDs must be unique within a response") {
    check_rejected_response(tool_response({
                                tool_call("duplicate", "one"),
                                tool_call("duplicate", "two"),
                            }),
                            ErrorCategory::protocol);
  }

  SECTION("arguments must be valid JSON") {
    check_rejected_response(tool_response({tool_call("call-1", "lookup", "{")}),
                            ErrorCategory::protocol);
  }

  SECTION("arguments must have an object root") {
    check_rejected_response(tool_response({tool_call("call-1", "lookup", "[]")}),
                            ErrorCategory::protocol);
  }
}

TEST_CASE("tool argument byte limit is exact and checked before dispatch") {
  auto tools = tool_policy();
  tools.max_argument_bytes = 7;

  SECTION("exact boundary is accepted") {
    auto machine = make_machine(retry_policy(), tools);
    begin(machine);
    const auto transition = machine.apply(scry::detail::ModelCompleted{
        .response = tool_response({tool_call("call-1", "tool", R"({"a":1})")}),
    });
    CHECK(only_command<scry::detail::PublishToolCall>(transition).call.arguments.text ==
          R"({"a":1})");
  }

  SECTION("one byte over is a resource failure") {
    auto machine = make_machine(retry_policy(), tools);
    begin(machine);
    const auto transition = machine.apply(scry::detail::ModelCompleted{
        .response = tool_response({tool_call("call-1", "tool", R"({"ab":1})")}),
    });
    CHECK(only_command<scry::detail::PublishError>(transition).error.category ==
          scry::ErrorCategory::resource_limit);
  }
}

TEST_CASE("tool-call IDs remain unique across the full turn") {
  auto machine = make_machine();
  begin(machine);
  auto published =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  static_cast<void>(only_command<scry::detail::PublishToolCall>(published));
  auto issue = machine.apply(result("call-1", "{}", at(1ms)));
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(issue));

  const auto duplicate =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  CHECK(only_command<scry::detail::PublishError>(duplicate).error.category ==
        scry::ErrorCategory::protocol);
}

TEST_CASE("unknown and duplicate tool results do not mutate pending state") {
  auto machine = make_machine();
  begin(machine);
  const auto published = machine.apply(scry::detail::ModelCompleted{
      .response = tool_response({
          tool_call("call-a", "one"),
          tool_call("call-b", "two"),
      }),
  });
  REQUIRE(published.commands.size() == 2);

  const auto unknown = machine.apply(result("unknown", "{}", at(1ms)));
  REQUIRE(unknown.diagnostic.has_value());
  CHECK(unknown.diagnostic->reason ==
        scry::detail::TransitionDiagnosticReason::unknown_tool_call);
  CHECK(unknown.commands.empty());
  CHECK(machine.phase() == scry::detail::MachinePhase::awaiting_tool);

  const auto accepted = machine.apply(result("call-a", "{}", at(2ms)));
  CHECK(accepted.commands.empty());
  const auto duplicate = machine.apply(result("call-a", "{}", at(3ms)));
  REQUIRE(duplicate.diagnostic.has_value());
  CHECK(duplicate.diagnostic->reason ==
        scry::detail::TransitionDiagnosticReason::duplicate_tool_result);
  CHECK(duplicate.commands.empty());

  const auto completed = machine.apply(result("call-b", "{}", at(4ms)));
  CHECK(only_command<scry::detail::IssueModelRequest>(completed).attempt == 2);
}

TEST_CASE("non-monotonic tool result is rejected without consuming its call") {
  auto machine = make_machine();
  begin(machine, 10ms);
  auto published =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  static_cast<void>(only_command<scry::detail::PublishToolCall>(published));

  const auto early = machine.apply(result("call-1", "{}", at(9ms)));
  REQUIRE(early.diagnostic.has_value());
  CHECK(early.diagnostic->reason ==
        scry::detail::TransitionDiagnosticReason::non_monotonic_time);
  CHECK(machine.phase() == scry::detail::MachinePhase::awaiting_tool);

  const auto due = machine.apply(result("call-1", "{}", at(10ms)));
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(due));
}

TEST_CASE("framework tool execution failure terminates from awaiting tool") {
  auto machine = make_machine();
  enter_awaiting_tool(machine);

  auto framework_error = error(scry::ErrorCategory::resource_limit, "result too large");
  framework_error.provider_request_id.clear();
  const auto failed = machine.apply(scry::detail::ToolExecutionFailed{
      .error = std::move(framework_error),
  });
  const auto& published = only_command<scry::detail::PublishError>(failed).error;
  CHECK(published.category == scry::ErrorCategory::resource_limit);
  CHECK(published.message == "result too large");
  CHECK(published.turn_id == turn_id);
  CHECK(published.attempt == 1);
  CHECK(published.provider_request_id == "tool-request");
  CHECK(machine.terminal_kind() == scry::detail::MachineTerminalKind::failed);
}

TEST_CASE("retry caps and elapsed windows reset for each model request") {
  auto retry = retry_policy();
  retry.max_attempts = 2;
  retry.max_elapsed = 500ms;
  auto machine = make_machine(retry);
  begin(machine, 0ms);

  auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(100ms),
  });
  auto wake = machine.apply(scry::detail::RetryWake{
      .observed_at = only_command<scry::detail::ScheduleRetryWake>(failed).deadline,
  });
  CHECK(only_command<scry::detail::IssueModelRequest>(wake).attempt == 2);

  auto published =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  static_cast<void>(only_command<scry::detail::PublishToolCall>(published));
  auto next = machine.apply(result("call-1", "{}", at(10s)));
  CHECK(only_command<scry::detail::IssueModelRequest>(next).attempt == 3);

  failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(10'100ms),
  });
  const auto& schedule = only_command<scry::detail::ScheduleRetryWake>(failed);
  CHECK(schedule.failed_attempt == 3);
  wake = machine.apply(scry::detail::RetryWake{.observed_at = schedule.deadline});
  CHECK(only_command<scry::detail::IssueModelRequest>(wake).attempt == 4);
  CHECK(machine.attempt_count() == 4);
}

TEST_CASE("model-request retries never redispatch completed tool calls") {
  auto machine = make_machine();
  begin(machine);
  auto published =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  static_cast<void>(only_command<scry::detail::PublishToolCall>(published));
  auto issue = machine.apply(result("call-1", "{}", at(1ms)));
  CHECK(only_command<scry::detail::IssueModelRequest>(issue).attempt == 2);

  const auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(2ms),
  });
  const auto deadline = only_command<scry::detail::ScheduleRetryWake>(failed).deadline;
  const auto retry = machine.apply(scry::detail::RetryWake{.observed_at = deadline});
  CHECK(only_command<scry::detail::IssueModelRequest>(retry).attempt == 3);
}

TEST_CASE("retry attempts reissue one shared request snapshot") {
  auto machine = make_machine();
  const auto first = machine.apply(scry::detail::BeginTurn{.observed_at = at(0ms)});
  const auto& issued = only_command<scry::detail::IssueModelRequest>(first);
  const auto* snapshot = issued.request.get();

  const auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(1ms),
  });
  const auto deadline = only_command<scry::detail::ScheduleRetryWake>(failed).deadline;
  const auto retry = machine.apply(scry::detail::RetryWake{.observed_at = deadline});
  const auto& reissued = only_command<scry::detail::IssueModelRequest>(retry);

  CHECK(reissued.attempt == 2);
  CHECK(reissued.request.get() == snapshot);
}

TEST_CASE("an issued request snapshot never observes later tool-round messages") {
  auto machine = make_machine();
  const auto first = machine.apply(scry::detail::BeginTurn{.observed_at = at(0ms)});
  // Retaining the snapshot models an attempt still reading it while the tool
  // round appends. Copy-on-write must reseat the machine, not this reader.
  const auto snapshot = only_command<scry::detail::IssueModelRequest>(first).request;
  REQUIRE(snapshot->messages.size() == 1);

  const auto published =
      machine.apply(scry::detail::ModelCompleted{.response = tool_response()});
  static_cast<void>(only_command<scry::detail::PublishToolCall>(published));
  const auto issued = machine.apply(result("call-1", R"({"ok":true})", at(1ms)));
  const auto& reissued = only_command<scry::detail::IssueModelRequest>(issued);

  CHECK(snapshot->messages.size() == 1);
  CHECK(reissued.request->messages.size() == 3);
  CHECK(reissued.request.get() != snapshot.get());
}
