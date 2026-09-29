#include "machine_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::detail::machine_test;

// Event replays for a turn sent with a response format. The machine sees the
// response tool through its request, publishes the model's call to it for the
// host to validate, and ends the turn only on an accepted lone answer.
namespace {

[[nodiscard]] TurnMachine
make_typed_machine(const ToolLoopPolicy tools = tool_policy()) {
  return {turn_id, typed_request(), retry_policy(), tools};
}

[[nodiscard]] ToolCallBlock answer_call(std::string id = "answer-1",
                                        std::string arguments = R"({"z":1,"a":true})") {
  return tool_call(std::move(id), "respond", std::move(arguments));
}

// A response carrying prose and a call to the response tool, finishing as the
// provider reports a forced tool call.
[[nodiscard]] ModelResponse answer_response(std::vector<ContentBlock> content = {
                                                TextBlock{.text = "Here it is."},
                                                answer_call()}) {
  return tool_response(std::move(content));
}

[[nodiscard]] std::vector<PublishToolCall>
published_calls(const TransitionResult& result) {
  REQUIRE(result.status == TransitionStatus::applied);
  std::vector<PublishToolCall> calls;
  for (const auto& command : result.commands) {
    REQUIRE(std::holds_alternative<PublishToolCall>(command));
    calls.push_back(std::get<PublishToolCall>(command));
  }
  return calls;
}

[[nodiscard]] ToolResultReady rejection(std::string id) {
  return result(std::move(id), R"({"error":"$.a must be a boolean"})", at(5ms), true);
}

} // namespace

TEST_CASE("a lone answer is published for validation and its acceptance commits it") {
  auto machine = make_typed_machine();
  begin(machine);
  const auto published = machine.apply(ModelCompleted{.response = answer_response()});
  const auto& candidate = only_command<PublishToolCall>(published);
  CHECK(candidate.role == ToolCallRole::answer);
  CHECK(candidate.call.name == "respond");
  // The machine canonicalizes the arguments before anything sees them.
  CHECK(candidate.call.arguments.text == R"({"a":true,"z":1})");
  CHECK(candidate.round == 1);
  CHECK(candidate.index == 0);
  CHECK(machine.phase() == MachinePhase::awaiting_tool);

  const auto accepted = machine.apply(AnswerAccepted{.call_id = "answer-1"});
  const auto& commit = only_command<CommitCompletion>(accepted);
  CHECK(commit.answered);
  CHECK(commit.finish_reason == scry::FinishReason::completed);
  CHECK(commit.tool_round_count == 0);
  CHECK(commit.tool_call_count == 0);
  CHECK(commit.answer_attempt_count == 1);
  CHECK(commit.usage.output_tokens == 3);
  CHECK(commit.provider_request_id == "tool-request");
  // The committed reply keeps its prose and ends with the answer as text, so no
  // tool call is left without a result.
  REQUIRE(commit.transcript.size() == 2);
  const auto& reply = commit.transcript.back();
  CHECK(reply.role == Role::assistant);
  REQUIRE(reply.content.size() == 2);
  CHECK(std::get<TextBlock>(reply.content[0]).text == "Here it is.");
  CHECK(std::get<TextBlock>(reply.content[1]).text == R"({"a":true,"z":1})");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("a rejected answer costs a round and the corrected answer completes") {
  auto machine = make_typed_machine();
  begin(machine);
  only_command<PublishToolCall>(
      machine.apply(ModelCompleted{.response = answer_response({answer_call()})}));

  const auto resent = machine.apply(rejection("answer-1"));
  const auto& issue = only_command<IssueModelRequest>(resent);
  // The rejected attempt is an ordinary round: the call and its error result.
  REQUIRE(issue.request->messages.size() == 3);
  const auto& results = issue.request->messages[2];
  CHECK(results.role == Role::user);
  const auto& error = std::get<ToolResultBlock>(results.content.front());
  CHECK(error.tool_call_id == "answer-1");
  CHECK(error.is_error);

  const auto second = machine.apply(ModelCompleted{
      .response = answer_response({answer_call("answer-2", R"({"a":false,"z":2})")})});
  const auto& candidate = only_command<PublishToolCall>(second);
  CHECK(candidate.role == ToolCallRole::answer);
  CHECK(candidate.round == 2);

  const auto accepted = machine.apply(AnswerAccepted{.call_id = "answer-2"});
  const auto& commit = only_command<CommitCompletion>(accepted);
  CHECK(commit.answered);
  CHECK(commit.tool_round_count == 1);
  CHECK(commit.tool_call_count == 0);
  CHECK(commit.answer_attempt_count == 2);
  CHECK(commit.attempt_count == 2);
  REQUIRE(commit.transcript.size() == 4);
  const auto& reply = commit.transcript.back();
  REQUIRE(reply.content.size() == 1);
  CHECK(std::get<TextBlock>(reply.content.front()).text == R"({"a":false,"z":2})");
}

TEST_CASE("an answer beside real tool calls is refused and the real calls dispatch") {
  auto machine = make_typed_machine();
  begin(machine);
  const auto published = machine.apply(ModelCompleted{
      .response = answer_response({tool_call("call-1"), answer_call()})});
  const auto calls = published_calls(published);
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].role == ToolCallRole::tool);
  CHECK(calls[0].round == 1);
  CHECK(calls[1].role == ToolCallRole::misplaced_answer);
  CHECK(calls[1].round == 1);
  CHECK(calls[1].index == 1);

  // Acceptance is not an answer to a misplaced call.
  CHECK(machine.apply(AnswerAccepted{.call_id = "answer-1"}).status ==
        TransitionStatus::event_not_allowed);

  static_cast<void>(machine.apply(result("call-1", R"({"x":1})", at(2ms))));
  only_command<IssueModelRequest>(machine.apply(rejection("answer-1")));

  only_command<PublishToolCall>(machine.apply(
      ModelCompleted{.response = answer_response({answer_call("answer-2")})}));
  const auto accepted = machine.apply(AnswerAccepted{.call_id = "answer-2"});
  const auto& commit = only_command<CommitCompletion>(accepted);
  CHECK(commit.tool_round_count == 1);
  CHECK(commit.tool_call_count == 1);
  CHECK(commit.answer_attempt_count == 2);
}

TEST_CASE("two answers in one response are both refused") {
  auto machine = make_typed_machine();
  begin(machine);
  const auto calls = published_calls(machine.apply(ModelCompleted{
      .response =
          answer_response({answer_call("answer-1"), answer_call("answer-2")})}));
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].role == ToolCallRole::misplaced_answer);
  CHECK(calls[1].role == ToolCallRole::misplaced_answer);
}

TEST_CASE("a typed turn whose response calls no tool fails with protocol") {
  SECTION("the server ignored the required tool choice") {
    auto machine = make_typed_machine();
    begin(machine);
    const auto failed =
        machine.apply(ModelCompleted{.response = final_response("I think yes.")});
    const auto& error = only_command<PublishError>(failed).error;
    CHECK(error.category == scry::ErrorCategory::protocol);
    CHECK(error.message == "model response did not call the response tool \"respond\"; "
                           "the server may not honor the required tool choice");
    CHECK(error.provider_request_id == "final-request");
    CHECK(error.turn_id == turn_id);
  }
  SECTION("the output ran out of tokens first") {
    auto machine = make_typed_machine();
    begin(machine);
    auto truncated = final_response("I think");
    truncated.finish_reason = scry::FinishReason::length;
    const auto failed = machine.apply(ModelCompleted{.response = truncated});
    const auto& error = only_command<PublishError>(failed).error;
    CHECK(error.category == scry::ErrorCategory::protocol);
    CHECK(error.message ==
          "model output reached its token limit before calling the response tool "
          "\"respond\"");
  }
}

TEST_CASE("a forced call reported with a plain stop is still a tool response") {
  auto machine = make_typed_machine();
  begin(machine);
  auto response = answer_response({answer_call()});
  response.finish_reason = scry::FinishReason::completed;
  const auto published = machine.apply(ModelCompleted{.response = response});
  CHECK(only_command<PublishToolCall>(published).role == ToolCallRole::answer);
}

namespace {

// One round, under each limit policy: a typed turn asked for an answer, so
// running out of rounds without one fails either way.
[[nodiscard]] std::vector<ToolLoopPolicy> one_round_policies() {
  std::vector<ToolLoopPolicy> policies;
  for (const auto policy :
       {scry::ToolRoundLimitPolicy::fail, scry::ToolRoundLimitPolicy::complete}) {
    auto limits = tool_policy();
    limits.max_rounds = 1;
    limits.limit_policy = policy;
    policies.push_back(limits);
  }
  return policies;
}

// Spends the turn's one round on a real tool.
void spend_the_round(TurnMachine& machine) {
  begin(machine);
  only_command<PublishToolCall>(
      machine.apply(ModelCompleted{.response = tool_response({tool_call()})}));
  only_command<IssueModelRequest>(machine.apply(result("call-1", "{}")));
}

} // namespace

TEST_CASE("a rejected answer at the round limit fails under either policy") {
  for (const auto& limits : one_round_policies()) {
    INFO(static_cast<int>(limits.limit_policy));
    auto machine = make_typed_machine(limits);
    begin(machine);
    only_command<PublishToolCall>(
        machine.apply(ModelCompleted{.response = answer_response({answer_call()})}));
    only_command<IssueModelRequest>(machine.apply(rejection("answer-1")));
    // The one round is spent, but a lone answer is still validated.
    only_command<PublishToolCall>(machine.apply(
        ModelCompleted{.response = answer_response({answer_call("answer-2")})}));
    const auto failed = machine.apply(rejection("answer-2"));
    const auto& error = only_command<PublishError>(failed).error;
    CHECK(error.category == scry::ErrorCategory::max_tool_rounds);
    CHECK(error.message ==
          "model exceeded the configured tool-round limit without a valid answer");
  }
}

TEST_CASE("tool calls at the round limit fail a typed turn under either policy") {
  for (const auto& limits : one_round_policies()) {
    INFO(static_cast<int>(limits.limit_policy));
    auto machine = make_typed_machine(limits);
    spend_the_round(machine);
    const auto failed = machine.apply(ModelCompleted{
        .response = tool_response({tool_call("call-2"), answer_call()})});
    CHECK(only_command<PublishError>(failed).error.category ==
          scry::ErrorCategory::max_tool_rounds);
  }
}

TEST_CASE("an accepted answer at the round limit completes") {
  for (const auto& limits : one_round_policies()) {
    INFO(static_cast<int>(limits.limit_policy));
    auto machine = make_typed_machine(limits);
    spend_the_round(machine);
    only_command<PublishToolCall>(
        machine.apply(ModelCompleted{.response = answer_response({answer_call()})}));
    const auto accepted = machine.apply(AnswerAccepted{.call_id = "answer-1"});
    const auto& commit = only_command<CommitCompletion>(accepted);
    CHECK(commit.finish_reason == scry::FinishReason::completed);
    CHECK(commit.tool_round_count == 1);
    CHECK(commit.tool_call_count == 1);
  }
}

TEST_CASE("cancellation while an answer is validated ends the turn uncommitted") {
  auto machine = make_typed_machine();
  begin(machine);
  only_command<PublishToolCall>(
      machine.apply(ModelCompleted{.response = answer_response()}));
  only_command<PublishCancelled>(machine.apply(CancelTurn{}));
  CHECK(machine.phase() == MachinePhase::terminal);
  CHECK(machine.apply(AnswerAccepted{.call_id = "answer-1"}).status ==
        TransitionStatus::ignored_terminal);
}

TEST_CASE("an answer verdict is refused where no candidate awaits it") {
  SECTION("an untyped turn's tool round") {
    auto machine = make_machine();
    enter_awaiting_tool(machine);
    CHECK(machine.apply(AnswerAccepted{.call_id = "call-1"}).status ==
          TransitionStatus::event_not_allowed);
  }
  SECTION("a different call") {
    auto machine = make_typed_machine();
    begin(machine);
    only_command<PublishToolCall>(
        machine.apply(ModelCompleted{.response = answer_response()}));
    CHECK(machine.apply(AnswerAccepted{.call_id = "answer-9"}).status ==
          TransitionStatus::unknown_tool_call);
    CHECK(machine.phase() == MachinePhase::awaiting_tool);
  }
  SECTION("while the model is still answering") {
    auto machine = make_typed_machine();
    begin(machine);
    CHECK(machine.apply(AnswerAccepted{.call_id = "answer-1"}).status ==
          TransitionStatus::event_not_allowed);
  }
}

TEST_CASE("a typed turn keeps the untyped loop for real tools") {
  auto machine = make_typed_machine();
  begin(machine);
  const auto published = machine.apply(ModelCompleted{.response = tool_response()});
  const auto& call = only_command<PublishToolCall>(published);
  CHECK(call.role == ToolCallRole::tool);
  CHECK(call.round == 1);
  only_command<IssueModelRequest>(machine.apply(result("call-1", R"({"x":1})")));
}

TEST_CASE(
    "a call named like the response tool is an ordinary tool in an untyped turn") {
  auto machine = make_machine();
  begin(machine);
  const auto published =
      machine.apply(ModelCompleted{.response = answer_response({answer_call()})});
  CHECK(only_command<PublishToolCall>(published).role == ToolCallRole::tool);
}
