#include "machine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::detail::machine_test;

namespace {

const Message& message_at(const std::vector<Message>& messages, const std::size_t index,
                          const Role role) {
  REQUIRE(index < messages.size());
  CHECK(messages[index].role == role);
  return messages[index];
}

[[nodiscard]] const ToolCallBlock* find_call(const std::vector<ContentBlock>& content,
                                             const std::string_view id) {
  for (const auto& block : content) {
    const auto* call = std::get_if<ToolCallBlock>(&block);
    if (call != nullptr && call->id == id) {
      return call;
    }
  }
  return nullptr;
}

void check_rejected_response(ModelResponse response,
                             const scry::ErrorCategory category) {
  const auto provider_request_id = response.provider_request_id;
  auto machine = make_machine();
  begin(machine);
  const auto transition =
      machine.apply(ModelCompleted{.response = std::move(response)});
  const auto& error = only_command<PublishError>(transition).error;
  CHECK(error.category == category);
  CHECK(error.turn_id == turn_id);
  CHECK(error.attempt == 1);
  CHECK(error.provider_request_id == provider_request_id);
}

} // namespace

TEST_CASE("out-of-order tool results produce one ordered result message") {
  auto machine = make_machine();
  begin(machine);
  const auto published = machine.apply(ModelCompleted{
      .response = tool_response({
          tool_call("call-a", "first"),
          tool_call("call-b", "second"),
      }),
  });
  REQUIRE(published.commands.size() == 2);

  const auto second = machine.apply(result("call-b", R"({"order":2})", at(2ms), true));
  CHECK(second.status == TransitionStatus::applied);
  CHECK(second.commands.empty());
  CHECK(machine.phase() == MachinePhase::awaiting_tool);

  const auto first = machine.apply(result("call-a", R"({"order":1})", at(3ms)));
  const auto& issue = only_command<IssueModelRequest>(first);
  REQUIRE(issue.request->messages.size() == 3);
  const auto& assistant = message_at(issue.request->messages, 1, Role::assistant);
  REQUIRE(assistant.content.size() == 2);
  const auto& results = message_at(issue.request->messages, 2, Role::user);
  REQUIRE(results.content.size() == 2);
  const auto& first_result = std::get<ToolResultBlock>(results.content[0]);
  const auto& second_result = std::get<ToolResultBlock>(results.content[1]);
  CHECK(first_result.tool_call_id == "call-a");
  CHECK(first_result.result.text == R"({"order":1})");
  CHECK_FALSE(first_result.is_error);
  CHECK(second_result.tool_call_id == "call-b");
  CHECK(second_result.result.text == R"({"order":2})");
  CHECK(second_result.is_error);
  CHECK(machine.attempt_count() == 2);
}

TEST_CASE("multi-round completion carries the transactional transcript and totals") {
  auto machine = make_machine();
  begin(machine);

  auto round_one = machine.apply(ModelCompleted{.response = tool_response()});
  only_command<PublishToolCall>(round_one);
  auto next = machine.apply(result("call-1", R"({"one":1})", at(10ms)));
  only_command<IssueModelRequest>(next);

  auto second_response = tool_response({tool_call("call-2", "second")});
  second_response.usage = {.input_tokens = 5, .output_tokens = 7};
  auto round_two =
      machine.apply(ModelCompleted{.response = std::move(second_response)});
  only_command<PublishToolCall>(round_two);
  next = machine.apply(result("call-2", R"({"two":2})", at(20ms)));
  only_command<IssueModelRequest>(next);

  const auto completed =
      machine.apply(ModelCompleted{.response = final_response("finished", 11, 13)});
  const auto& commit = only_command<CommitCompletion>(completed);

  CHECK(commit.attempt_count == 3);
  CHECK(commit.usage.input_tokens == 18);
  CHECK(commit.usage.output_tokens == 23);
  CHECK(commit.finish_reason == scry::FinishReason::completed);
  CHECK(commit.provider_request_id == "final-request");
  // The user message the turn opened with, then one assistant/results pair per
  // tool round, then the final assistant reply.
  REQUIRE(commit.transcript.size() == 6);
  message_at(commit.transcript, 0, Role::user);
  message_at(commit.transcript, 1, Role::assistant);
  message_at(commit.transcript, 2, Role::user);
  message_at(commit.transcript, 3, Role::assistant);
  message_at(commit.transcript, 4, Role::user);
  const auto& final = message_at(commit.transcript, 5, Role::assistant);
  REQUIRE(final.content.size() == 1);
  CHECK(std::get<TextBlock>(final.content.front()).text == "finished");
}

TEST_CASE("published calls carry their round and batch position") {
  auto machine = make_machine();
  begin(machine);

  // Text between the calls neither dispatches nor takes a batch position.
  const auto round_one = machine.apply(ModelCompleted{
      .response = tool_response({
          TextBlock{.text = "I will look twice."},
          tool_call("call-a", "first"),
          TextBlock{.text = "Then compare."},
          tool_call("call-b", "second"),
      }),
  });
  REQUIRE(round_one.status == TransitionStatus::applied);
  REQUIRE(round_one.commands.size() == 2);
  const auto& first = std::get<PublishToolCall>(round_one.commands[0]);
  const auto& second = std::get<PublishToolCall>(round_one.commands[1]);
  CHECK(first.turn_id == turn_id);
  CHECK(first.call.id == "call-a");
  CHECK(first.call.name == "first");
  CHECK(first.round == 1);
  CHECK(first.index == 0);
  CHECK(second.call.id == "call-b");
  CHECK(second.call.name == "second");
  CHECK(second.round == 1);
  CHECK(second.index == 1);
  CHECK(machine.phase() == MachinePhase::awaiting_tool);

  static_cast<void>(machine.apply(result("call-a", "{}", at(1ms))));
  const auto issued = machine.apply(result("call-b", "{}", at(2ms)));
  only_command<IssueModelRequest>(issued);

  const auto round_two = machine.apply(ModelCompleted{
      .response = tool_response({tool_call("call-c", "third")}),
  });
  const auto& third = only_command<PublishToolCall>(round_two);
  CHECK(third.round == 2);
  CHECK(third.index == 0);

  const auto next = machine.apply(result("call-c", "{}", at(3ms)));
  only_command<IssueModelRequest>(next);
  const auto completed = machine.apply(ModelCompleted{.response = final_response()});
  const auto& commit = only_command<CommitCompletion>(completed);
  CHECK(commit.tool_round_count == 2);
  CHECK(commit.tool_call_count == 3);
}

TEST_CASE("tool-round cap fails before publishing any call from excess round") {
  auto tools = tool_policy();
  tools.max_rounds = 1;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  auto first = machine.apply(ModelCompleted{
      .response = tool_response({
          tool_call("call-1", "first"),
          tool_call("call-1b", "second"),
      }),
  });
  REQUIRE(first.commands.size() == 2);
  auto partial = machine.apply(result("call-1", "{}", at(1ms)));
  CHECK(partial.commands.empty());
  auto issue = machine.apply(result("call-1b", "{}", at(2ms)));
  only_command<IssueModelRequest>(issue);

  const auto excess = machine.apply(ModelCompleted{
      .response = tool_response({tool_call("call-2", "again")}),
  });
  const auto& error = only_command<PublishError>(excess).error;
  CHECK(error.category == scry::ErrorCategory::max_tool_rounds);
  CHECK(error.turn_id == turn_id);
  CHECK(error.attempt == 2);
  CHECK(error.provider_request_id == "tool-request");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("zero tool-round cap rejects the first call batch") {
  auto tools = tool_policy();
  tools.max_rounds = 0;
  auto machine = make_machine(retry_policy(), tools);
  begin(machine);

  const auto transition = machine.apply(ModelCompleted{.response = tool_response()});
  CHECK(only_command<PublishError>(transition).error.category ==
        scry::ErrorCategory::max_tool_rounds);
}

TEST_CASE("model completion validates finish and content consistency") {
  using scry::ErrorCategory;

  SECTION("tool-use finish requires at least one call") {
    check_rejected_response(tool_response({TextBlock{.text = "none"}}),
                            ErrorCategory::protocol);
  }

  SECTION("calls require a tool-use finish") {
    auto response = tool_response();
    response.finish_reason = scry::FinishReason::completed;
    check_rejected_response(std::move(response), ErrorCategory::protocol);
  }

  SECTION("assistant responses cannot contain tool results") {
    auto response = final_response();
    response.content = {ToolResultBlock{
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
    const auto transition = machine.apply(ModelCompleted{
        .response = tool_response({tool_call("call-1", "tool", R"({"a":1})")}),
    });
    CHECK(only_command<PublishToolCall>(transition).call.arguments.text ==
          R"({"a":1})");
  }

  SECTION("one byte over is a resource failure") {
    auto machine = make_machine(retry_policy(), tools);
    begin(machine);
    const auto transition = machine.apply(ModelCompleted{
        .response = tool_response({tool_call("call-1", "tool", R"({"ab":1})")}),
    });
    CHECK(only_command<PublishError>(transition).error.category ==
          scry::ErrorCategory::resource_limit);
  }
}

TEST_CASE("tool-call IDs remain unique across the full turn") {
  auto machine = make_machine();
  begin(machine);
  auto published = machine.apply(ModelCompleted{.response = tool_response()});
  only_command<PublishToolCall>(published);
  auto issue = machine.apply(result("call-1", "{}", at(1ms)));
  only_command<IssueModelRequest>(issue);

  const auto duplicate = machine.apply(ModelCompleted{.response = tool_response()});
  CHECK(only_command<PublishError>(duplicate).error.category ==
        scry::ErrorCategory::protocol);
}

TEST_CASE("framework tool execution failure terminates from awaiting tool") {
  auto machine = make_machine();
  enter_awaiting_tool(machine);

  auto framework_error = error(scry::ErrorCategory::resource_limit, "result too large");
  framework_error.provider_request_id.clear();
  const auto failed = machine.apply(ToolExecutionFailed{
      .error = std::move(framework_error),
  });
  const auto& published = only_command<PublishError>(failed).error;
  CHECK(published.category == scry::ErrorCategory::resource_limit);
  CHECK(published.message == "result too large");
  CHECK(published.turn_id == turn_id);
  CHECK(published.attempt == 1);
  CHECK(published.provider_request_id == "tool-request");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("retry caps and elapsed windows reset for each model request") {
  auto retry = retry_policy();
  retry.max_attempts = 2;
  retry.max_elapsed = 500ms;
  auto machine = make_machine(retry);
  begin(machine, 0ms);

  auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(100ms),
  });
  auto wake = machine.apply(RetryWake{
      .observed_at = only_command<ScheduleRetryWake>(failed).deadline,
  });
  only_command<IssueModelRequest>(wake);
  CHECK(machine.attempt_count() == 2);

  auto published = machine.apply(ModelCompleted{.response = tool_response()});
  only_command<PublishToolCall>(published);
  auto next = machine.apply(result("call-1", "{}", at(10s)));
  only_command<IssueModelRequest>(next);
  CHECK(machine.attempt_count() == 3);

  failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(10'100ms),
  });
  const auto& schedule = only_command<ScheduleRetryWake>(failed);
  wake = machine.apply(RetryWake{.observed_at = schedule.deadline});
  only_command<IssueModelRequest>(wake);
  CHECK(machine.attempt_count() == 4);
}

TEST_CASE("retry attempts reissue one shared request snapshot") {
  auto machine = make_machine();
  const auto first = machine.apply(BeginTurn{.observed_at = at(0ms)});
  const auto& issued = only_command<IssueModelRequest>(first);
  const auto* snapshot = issued.request.get();

  const auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(1ms),
  });
  const auto deadline = only_command<ScheduleRetryWake>(failed).deadline;
  const auto retry = machine.apply(RetryWake{.observed_at = deadline});
  const auto& reissued = only_command<IssueModelRequest>(retry);

  CHECK(machine.attempt_count() == 2);
  CHECK(reissued.request.get() == snapshot);
}

TEST_CASE("an issued request snapshot never observes later tool-round messages") {
  auto machine = make_machine();
  const auto first = machine.apply(BeginTurn{.observed_at = at(0ms)});
  // Retaining the snapshot models an attempt still reading it while the tool
  // round appends. Copy-on-write must reseat the machine, not this reader.
  const auto snapshot = only_command<IssueModelRequest>(first).request;
  REQUIRE(snapshot->messages.size() == 1);

  const auto published = machine.apply(ModelCompleted{.response = tool_response()});
  only_command<PublishToolCall>(published);
  const auto issued = machine.apply(result("call-1", R"({"ok":true})", at(1ms)));
  const auto& reissued = only_command<IssueModelRequest>(issued);

  CHECK(snapshot->messages.size() == 1);
  CHECK(reissued.request->messages.size() == 3);
  CHECK(reissued.request.get() != snapshot.get());
}

// The request the machine resends already holds every committed message, so the
// commit is that vector with the final reply appended rather than a second
// transcript assembled alongside it.
TEST_CASE("a completed transcript is the last issued request plus the final reply") {
  auto machine = make_machine();
  begin(machine);

  auto round_one = machine.apply(ModelCompleted{.response = tool_response()});
  only_command<PublishToolCall>(round_one);
  auto issued = machine.apply(result("call-1", R"({"one":1})", at(10ms)));
  const auto& issue = only_command<IssueModelRequest>(issued);
  // The machine and this command are the request's only owners, so releasing
  // the command below lets the commit move the transcript instead of copying it.
  CHECK(issue.request.use_count() == 2);
  const auto sent = issue.request->messages;
  issued.commands.clear();

  const auto completed =
      machine.apply(ModelCompleted{.response = final_response("finished")});
  const auto& commit = only_command<CommitCompletion>(completed);

  REQUIRE(commit.transcript.size() == sent.size() + 1);
  for (std::size_t index = 0; index < sent.size(); ++index) {
    CHECK(commit.transcript[index].role == sent[index].role);
    CHECK(message_payload_bytes(commit.transcript[index]) ==
          message_payload_bytes(sent[index]));
  }
  const auto& reply = message_at(commit.transcript, sent.size(), Role::assistant);
  REQUIRE(reply.content.size() == 1);
  CHECK(std::get<TextBlock>(reply.content.front()).text == "finished");
}

// The machine keeps one canonicalized copy of each call in the assistant message
// it commits and hands the dispatch its own copy of the same block. Nothing may
// drift between the two: a handler and the history must see identical arguments.
TEST_CASE("dispatched tool calls and the committed transcript carry the same bytes") {
  auto machine = make_machine();
  begin(machine);

  const auto published = machine.apply(ModelCompleted{
      .response = tool_response({
          TextBlock{.text = "Looking twice."},
          tool_call("call-a", "first", R"({ "b" : 2, "a" : 1 })"),
          tool_call("call-b", "second", R"({"outer":{ "k" : [1, 2] }})"),
      }),
  });
  REQUIRE(published.commands.size() == 2);
  std::vector<ToolCallBlock> dispatched;
  for (const auto& command : published.commands) {
    dispatched.push_back(std::get<PublishToolCall>(command).call);
  }
  CHECK(dispatched[0].id == "call-a");
  CHECK(dispatched[0].arguments.text == R"({"a":1,"b":2})");
  CHECK(dispatched[1].id == "call-b");

  static_cast<void>(machine.apply(result("call-a", R"({"ok":1})", at(1ms))));
  const auto issued = machine.apply(result("call-b", R"({"ok":2})", at(2ms)));
  only_command<IssueModelRequest>(issued);
  const auto completed = machine.apply(ModelCompleted{.response = final_response()});
  const auto& commit = only_command<CommitCompletion>(completed);

  const auto& assistant = message_at(commit.transcript, 1, Role::assistant);
  REQUIRE(assistant.content.size() == 3);
  for (const auto& call : dispatched) {
    const auto* committed = find_call(assistant.content, call.id);
    REQUIRE(committed != nullptr);
    CHECK(committed->name == call.name);
    CHECK(committed->arguments.text == call.arguments.text);
  }
}

// The pending round holds call IDs, not the blocks themselves, so matching a
// result must depend on the ID alone: two calls that differ only by ID stay
// distinct, and an ID from a finished round is unknown to the current one.
TEST_CASE("tool results are matched by call ID alone") {
  auto machine = make_machine();
  begin(machine);
  const auto published = machine.apply(ModelCompleted{
      .response = tool_response({
          tool_call("call-a", "lookup", R"({"x":1})"),
          tool_call("call-b", "lookup", R"({"x":1})"),
      }),
  });
  REQUIRE(published.commands.size() == 2);

  const auto unknown = machine.apply(result("call-c", "{}", at(1ms)));
  CHECK(unknown.status == TransitionStatus::unknown_tool_call);
  CHECK(unknown.commands.empty());

  const auto accepted = machine.apply(result("call-b", R"({"got":"b"})", at(1ms)));
  CHECK(accepted.commands.empty());
  const auto duplicate = machine.apply(result("call-b", R"({"got":"again"})", at(2ms)));
  CHECK(duplicate.status == TransitionStatus::duplicate_tool_result);
  CHECK(duplicate.commands.empty());

  const auto issued = machine.apply(result("call-a", R"({"got":"a"})", at(2ms)));
  const auto& issue = only_command<IssueModelRequest>(issued);
  const auto& results = message_at(issue.request->messages, 2, Role::user);
  REQUIRE(results.content.size() == 2);
  CHECK(std::get<ToolResultBlock>(results.content[0]).result.text == R"({"got":"a"})");
  CHECK(std::get<ToolResultBlock>(results.content[1]).result.text == R"({"got":"b"})");

  const auto round_two = machine.apply(ModelCompleted{
      .response = tool_response({tool_call("call-d", "lookup")}),
  });
  only_command<PublishToolCall>(round_two);
  const auto stale = machine.apply(result("call-a", "{}", at(3ms)));
  CHECK(stale.status == TransitionStatus::unknown_tool_call);
  CHECK(machine.phase() == MachinePhase::awaiting_tool);
}
