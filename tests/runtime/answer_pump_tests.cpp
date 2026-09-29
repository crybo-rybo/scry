#include "runtime_test_support.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

using namespace scry::test_support;
using scry::detail::ToolCallRole;

// The host side of a typed turn: the route validates a candidate answer on the
// update() thread and posts the verdict, and the pump turns an answered
// completion into Completion::structured.
namespace {

// The next command the pump posted, which has to be an answer acceptance.
[[nodiscard]] scry::detail::AnswerAcceptedCommand pop_acceptance(PumpFixture& fixture) {
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  auto* const accepted = std::get_if<scry::detail::AnswerAcceptedCommand>(&*command);
  REQUIRE(accepted);
  return std::move(*accepted);
}

// Callbacks that record whether any host tool hook saw a call.
struct HookCounts {
  std::size_t requests{};
  std::size_t observations{};
};

[[nodiscard]] scry::TurnCallbacks counting_hooks(HookCounts& counts) {
  return {
      .on_tool_request =
          [&counts](const scry::ToolRequest&) -> std::optional<scry::ToolRejection> {
        ++counts.requests;
        return std::nullopt;
      },
      .on_tool_call = [&counts](const scry::ToolCall&) { ++counts.observations; },
  };
}

[[nodiscard]] scry::AnswerValidator rejecting(std::string model_message) {
  return [model_message = std::move(model_message)](const scry::Json&) -> scry::Status {
    return std::unexpected(scry::Error{
        .category = scry::ErrorCategory::invalid_argument,
        .message = "host-only diagnostic",
        .model_message = model_message,
    });
  };
}

} // namespace

TEST_CASE("the answer validator runs on the update thread and accepts the answer") {
  PumpFixture fixture;
  HookCounts hooks;
  std::thread::id validator_thread{};
  std::string validated;
  const auto route = fixture.add(
      401, {
               .callbacks = counting_hooks(hooks),
               .validate_answer = [&](const scry::Json& answer) -> scry::Status {
                 validator_thread = std::this_thread::get_id();
                 validated = answer.text;
                 return {};
               },
           });
  REQUIRE(fixture.events->push(answer_event(route->id()), 1024));

  CHECK(fixture.pump.update({}).callbacks_delivered == 1);

  CHECK(validator_thread == std::this_thread::get_id());
  CHECK(validated == R"({"supported":true})");
  const auto accepted = pop_acceptance(fixture);
  CHECK(accepted.turn_id == route->id());
  CHECK(accepted.call_id == "answer-1");
  // A response-tool call is not a host tool call.
  CHECK(hooks.requests == 0);
  CHECK(hooks.observations == 0);
  CHECK_FALSE(fixture.commands->try_pop());
}

TEST_CASE("an empty validator accepts any answer") {
  PumpFixture fixture;
  const auto route = fixture.add(402);
  REQUIRE(fixture.events->push(answer_event(route->id()), 1024));

  static_cast<void>(fixture.pump.update({}));

  CHECK(pop_acceptance(fixture).call_id == "answer-1");
}

TEST_CASE("a rejected answer gives the model the validator's model_message") {
  PumpFixture fixture;
  HookCounts hooks;
  const auto route = fixture.add(
      403, {
               .callbacks = counting_hooks(hooks),
               .validate_answer = rejecting("$.supported is a required member"),
           });
  REQUIRE(fixture.events->push(answer_event(route->id()), 1024));

  static_cast<void>(fixture.pump.update({}));

  const auto result = pop_tool_result(fixture);
  REQUIRE(result);
  CHECK(result->tool_call_id == "answer-1");
  CHECK(result->is_error);
  CHECK(result->result.text == R"({"error":"$.supported is a required member"})");
  CHECK(hooks.observations == 0);
}

TEST_CASE("a validator that publishes nothing or throws is answered like a handler") {
  SECTION("an error without a model_message") {
    PumpFixture fixture;
    const auto route = fixture.add(404, {.validate_answer = rejecting("")});
    REQUIRE(fixture.events->push(answer_event(route->id()), 1024));
    static_cast<void>(fixture.pump.update({}));
    const auto result = pop_tool_result(fixture);
    REQUIRE(result);
    CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  }
  SECTION("a throw") {
    PumpFixture fixture;
    const auto route =
        fixture.add(405, {.validate_answer = [](const scry::Json&) -> scry::Status {
                      throw std::runtime_error{"secret detail"};
                    }});
    REQUIRE(fixture.events->push(answer_event(route->id()), 1024));
    static_cast<void>(fixture.pump.update({}));
    const auto result = pop_tool_result(fixture);
    REQUIRE(result);
    CHECK(result->is_error);
    CHECK(result->result.text == R"({"error":"tool handler returned an error"})");
  }
}

TEST_CASE("a misplaced answer is refused without its validator or the call limit") {
  PumpFixture fixture;
  HookCounts hooks;
  std::size_t validations = 0;
  std::size_t handler_calls = 0;
  const scry::detail::ToolSnapshot tools{registered_tool(
      "forecast", [&handler_calls](scry::Json) -> scry::Result<scry::Json> {
        ++handler_calls;
        return scry::Json{.text = "{}"};
      })};
  const auto route = fixture.add(
      406, {
               .tools = frozen_tools(tools),
               .max_tool_calls = 1,
               .callbacks = counting_hooks(hooks),
               .validate_answer = [&validations](const scry::Json&) -> scry::Status {
                 ++validations;
                 return {};
               },
           });
  REQUIRE(fixture.events->push(
      answer_event(route->id(), ToolCallRole::misplaced_answer, "answer-1"), 1024));
  REQUIRE(fixture.events->push(tool_event(route->id(), "forecast", "call-2"), 1024));

  static_cast<void>(fixture.pump.update({}));

  const auto refusal = pop_tool_result(fixture);
  REQUIRE(refusal);
  CHECK(refusal->tool_call_id == "answer-1");
  CHECK(refusal->is_error);
  CHECK(
      refusal->result.text ==
      R"({"error":"call respond exactly once, on its own, after your other tool calls have returned"})");
  CHECK(validations == 0);
  // The refused answer spent none of the per-turn limit, so the real call runs.
  const auto real = pop_tool_result(fixture);
  REQUIRE(real);
  CHECK_FALSE(real->is_error);
  CHECK(handler_calls == 1);
  CHECK(hooks.requests == 1);
  CHECK(hooks.observations == 1);
}

TEST_CASE("a validator that cancels the turn posts no verdict") {
  PumpFixture fixture;
  std::shared_ptr<scry::detail::TurnRoute> route;
  route =
      fixture.add(407, {.validate_answer = [&route](const scry::Json&) -> scry::Status {
                    static_cast<void>(route->cancel());
                    return {};
                  }});
  REQUIRE(fixture.events->push(answer_event(route->id()), 1024));

  static_cast<void>(fixture.pump.update({}));

  // The cancellation itself is the only command: no verdict follows it.
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::CancelTurnCommand>(*command));
  CHECK_FALSE(fixture.commands->try_pop());
}

TEST_CASE("an answer of a turn cancelled from a callback is never validated") {
  PumpFixture fixture;
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t validations = 0;
  route = fixture.add(
      408, {
               .callbacks = {.on_text_delta =
                                 [&route](std::string_view) {
                                   static_cast<void>(route->cancel());
                                 }},
               .validate_answer = [&validations](const scry::Json&) -> scry::Status {
                 ++validations;
                 return {};
               },
           });
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "thinking"}, 1024));
  REQUIRE(fixture.events->push(answer_event(route->id()), 1024));

  static_cast<void>(fixture.pump.update({}));

  CHECK(validations == 0);
  auto command = fixture.commands->try_pop();
  REQUIRE(command);
  CHECK(std::holds_alternative<scry::detail::CancelTurnCommand>(*command));
  CHECK_FALSE(fixture.commands->try_pop());
}

TEST_CASE("a turn disconnected from a callback still validates its answer") {
  PumpFixture fixture;
  std::shared_ptr<scry::detail::TurnRoute> route;
  std::size_t validations = 0;
  route = fixture.add(
      409, {
               .callbacks = {.on_text_delta =
                                 [&route](std::string_view) {
                                   static_cast<void>(route->disconnect());
                                 }},
               .validate_answer = [&validations](const scry::Json&) -> scry::Status {
                 ++validations;
                 return {};
               },
           });
  REQUIRE(fixture.events->push(
      scry::detail::TextDeltaEvent{.turn_id = route->id(), .text = "thinking"}, 1024));
  REQUIRE(fixture.events->push(answer_event(route->id()), 1024));

  static_cast<void>(fixture.pump.update({}));

  // Disconnecting stops delivery, not the turn: the answer is still checked.
  CHECK(validations == 1);
  CHECK(pop_acceptance(fixture).call_id == "answer-1");
}

TEST_CASE("a validator that disconnects its turn still posts the verdict") {
  PumpFixture fixture;
  std::shared_ptr<scry::detail::TurnRoute> route;
  route = fixture.add(
      410, {
               .callbacks = {.on_finished = [](scry::Result<scry::Completion>) {}},
               .validate_answer = [&route](const scry::Json&) -> scry::Status {
                 static_cast<void>(route->disconnect());
                 return {};
               },
           });
  REQUIRE(fixture.events->push(answer_event(route->id()), 1024));

  static_cast<void>(fixture.pump.update({}));

  CHECK(pop_acceptance(fixture).call_id == "answer-1");
}

TEST_CASE("an answered completion carries the answer apart from the prose") {
  PumpFixture fixture;
  std::optional<scry::Result<scry::Completion>> finished;
  const auto route = fixture.add(
      411,
      {.callbacks = {.on_finished = [&finished](scry::Result<scry::Completion> result) {
         finished = std::move(result);
       }}});
  auto completion = completion_event(route->id(), {.text = "Here it is."});
  completion.transcript.back().content.emplace_back(
      scry::detail::TextBlock{.text = R"({"supported":true})"});
  completion.answered = true;
  completion.answer_attempt_count = 2;
  REQUIRE(fixture.events->push(std::move(completion), 4096));

  static_cast<void>(fixture.pump.update({}));

  REQUIRE(finished);
  REQUIRE(*finished);
  const auto& delivered = **finished;
  REQUIRE(delivered.structured);
  CHECK(delivered.structured->text == R"({"supported":true})");
  CHECK(delivered.text == "Here it is.");
  CHECK(delivered.answer_attempt_count == 2);
  // History keeps the answer as the reply's final text block.
  const auto& history = *fixture.conversation->messages;
  REQUIRE(history.size() == 2);
  REQUIRE(history.back().content.size() == 2);
  CHECK(std::get<scry::detail::TextBlock>(history.back().content.back()).text ==
        R"({"supported":true})");
}

TEST_CASE("an unanswered completion carries no structured answer") {
  PumpFixture fixture;
  std::optional<scry::Result<scry::Completion>> finished;
  const auto route = fixture.add(
      412,
      {.callbacks = {.on_finished = [&finished](scry::Result<scry::Completion> result) {
         finished = std::move(result);
       }}});
  REQUIRE(fixture.events->push(completion_event(route->id()), 4096));

  static_cast<void>(fixture.pump.update({}));

  REQUIRE(finished);
  REQUIRE(*finished);
  CHECK_FALSE((*finished)->structured);
  CHECK((*finished)->text == "done");
  CHECK((*finished)->answer_attempt_count == 0);
}
