#pragma once

#include "core/retry.hpp"
#include "machine/turn_machine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <scry/error.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::detail;

namespace scry::detail::machine_test {

inline constexpr TurnId turn_id{42};

[[nodiscard]] inline MachineTimePoint at(const std::chrono::milliseconds elapsed) {
  return MachineTimePoint{elapsed};
}

[[nodiscard]] inline ModelRequest request() {
  return {
      .system_prompt = "Be concise.",
      .messages =
          {
              {
                  .role = Role::user,
                  .content = {TextBlock{.text = "Hello"}},
              },
          },
  };
}

[[nodiscard]] inline RetryPolicy retry_policy() {
  return {
      .max_attempts = 3,
      .initial_backoff = 100ms,
      .max_backoff = 2s,
      .max_elapsed = 10s,
      .jitter_ratio = 0.0,
  };
}

[[nodiscard]] inline ToolLoopPolicy tool_policy() {
  return {
      .max_rounds = 3,
      .max_argument_bytes = 1024,
  };
}

// A well-formed plain-text completion. Every committed message needs at least
// one non-empty block, so an empty ModelResponse is not a completion any
// machine will accept.
[[nodiscard]] inline ModelResponse text_response(std::string text = "answer") {
  return {
      .content = {TextBlock{.text = std::move(text)}},
      .finish_reason = FinishReason::completed,
  };
}

[[nodiscard]] inline ToolCallBlock tool_call(std::string id = "call-1",
                                             std::string name = "lookup",
                                             std::string arguments = R"({"x":1})") {
  return {
      .id = std::move(id),
      .name = std::move(name),
      .arguments = Json{.text = std::move(arguments)},
  };
}

[[nodiscard]] inline ModelResponse tool_response(std::vector<ContentBlock> content = {
                                                     tool_call()}) {
  return {
      .content = std::move(content),
      .finish_reason = FinishReason::tool_use,
      .usage = {.input_tokens = 2, .output_tokens = 3},
      .provider_request_id = "tool-request",
  };
}

[[nodiscard]] inline Error error(const ErrorCategory category,
                                 std::string message = "attempt failed") {
  return {
      .category = category,
      .retryable = is_retryable(category),
      .message = std::move(message),
      .provider_detail = "sanitized detail",
      .provider_request_id = "request-123",
  };
}

[[nodiscard]] inline ToolResultReady
result(std::string id, std::string json, const MachineTimePoint observed_at = at(1ms),
       const bool is_error = false) {
  return {
      .result =
          {
              .tool_call_id = std::move(id),
              .result = Json{.text = std::move(json)},
              .is_error = is_error,
          },
      .observed_at = observed_at,
  };
}

[[nodiscard]] inline ModelResponse
final_response(std::string text = "done", const std::uint64_t input_tokens = 7,
               const std::uint64_t output_tokens = 11) {
  return {
      .content = {TextBlock{.text = std::move(text)}},
      .finish_reason = FinishReason::completed,
      .usage = {.input_tokens = input_tokens, .output_tokens = output_tokens},
      .provider_request_id = "final-request",
  };
}

// Asserts the transition applied and issued exactly one Command. Callers that only
// need the assertion discard the reference.
template <typename Command>
const Command& only_command(const TransitionResult& result) {
  REQUIRE(result.status == TransitionStatus::applied);
  REQUIRE(result.commands.size() == 1);
  REQUIRE(std::holds_alternative<Command>(result.commands.front()));
  return std::get<Command>(result.commands.front());
}

[[nodiscard]] inline TurnMachine
make_machine(const RetryPolicy policy = retry_policy(),
             const ToolLoopPolicy tools = tool_policy()) {
  return {turn_id, request(), policy, tools};
}

inline void begin(TurnMachine& machine, const std::chrono::milliseconds elapsed = 0ms) {
  const auto result = machine.apply(BeginTurn{.observed_at = at(elapsed)});
  only_command<IssueModelRequest>(result);
}

inline void enter_streaming(TurnMachine& machine) {
  begin(machine);
  const auto result = machine.apply(ModelTextDelta{.text = "first"});
  only_command<PublishTextDelta>(result);
}

inline void enter_retry_wait(TurnMachine& machine) {
  begin(machine);
  const auto result = machine.apply(AttemptFailed{
      .error = error(ErrorCategory::network),
      .observed_at = at(0ms),
  });
  only_command<ScheduleRetryWake>(result);
}

inline void enter_awaiting_tool(TurnMachine& machine) {
  begin(machine);
  const auto result = machine.apply(ModelCompleted{.response = tool_response()});
  only_command<PublishToolCall>(result);
}

[[nodiscard]] inline bool is_terminal_command(const MachineCommand& command) {
  return std::holds_alternative<CommitCompletion>(command) ||
         std::holds_alternative<PublishError>(command) ||
         std::holds_alternative<PublishCancelled>(command);
}

// One sample of every event, in MachineEvent's alternative order.
[[nodiscard]] inline std::vector<MachineEvent> sample_events() {
  return {
      BeginTurn{.observed_at = at(0ms)},
      ModelTextDelta{.text = "delta"},
      ModelSemanticOutput{},
      ModelCompleted{.response = text_response()},
      AttemptFailed{
          .error = error(ErrorCategory::protocol),
          .observed_at = at(0ms),
      },
      RetryWake{.observed_at = at(100ms)},
      result("call-1", "{}", at(0ms)),
      ToolExecutionFailed{.error = error(ErrorCategory::resource_limit)},
      CancelTurn{},
  };
}

// The sample events a phase must reject: every one but the Accepted kinds.
template <typename... Accepted>
[[nodiscard]] std::vector<MachineEvent> events_except() {
  auto events = sample_events();
  std::erase_if(events, [](const MachineEvent& event) {
    return (std::holds_alternative<Accepted>(event) || ...);
  });
  return events;
}

} // namespace scry::detail::machine_test
