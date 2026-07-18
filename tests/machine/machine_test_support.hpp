#pragma once

#include "machine/turn_machine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <scry/error.hpp>
#include <string>
#include <utility>
#include <variant>

using namespace std::chrono_literals;

namespace scry::detail::machine_test {

inline constexpr TurnId turn_id{42};

[[nodiscard]] inline MachineTimePoint at(const std::chrono::milliseconds elapsed) {
  return MachineTimePoint{elapsed};
}

[[nodiscard]] inline ModelRequest request() {
  return {
      .model = "test-model",
      .system_prompt = "Be concise.",
      .messages =
          {
              {
                  .role = Role::user,
                  .content = {TextBlock{.text = "Hello"}},
              },
          },
      .streaming = true,
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

[[nodiscard]] inline Error error(const ErrorCategory category,
                                 std::string message = "attempt failed") {
  return {
      .category = category,
      .message = std::move(message),
      .provider_detail = "sanitized detail",
      .provider_request_id = "request-123",
  };
}

template <typename Command>
[[nodiscard]] const Command& only_command(const TransitionResult& result) {
  REQUIRE(result.status == TransitionStatus::applied);
  REQUIRE_FALSE(result.diagnostic.has_value());
  REQUIRE(result.commands.size() == 1);
  REQUIRE(std::holds_alternative<Command>(result.commands.front()));
  return std::get<Command>(result.commands.front());
}

[[nodiscard]] inline TurnMachine
make_machine(const RetryPolicy policy = retry_policy()) {
  return {turn_id, request(), policy};
}

inline void begin(TurnMachine& machine, const std::chrono::milliseconds elapsed = 0ms) {
  const auto result = machine.apply(BeginTurn{.observed_at = at(elapsed)});
  static_cast<void>(only_command<IssueModelRequest>(result));
}

inline void enter_streaming(TurnMachine& machine) {
  begin(machine);
  const auto result = machine.apply(ModelTextDelta{.text = "first"});
  static_cast<void>(only_command<PublishTextDelta>(result));
}

inline void enter_retry_wait(TurnMachine& machine) {
  begin(machine);
  const auto result = machine.apply(AttemptFailed{
      .error = error(ErrorCategory::network),
      .observed_at = at(0ms),
  });
  static_cast<void>(only_command<ScheduleRetryWake>(result));
}

[[nodiscard]] inline bool is_terminal_command(const MachineCommand& command) {
  return std::holds_alternative<CommitCompletion>(command) ||
         std::holds_alternative<PublishError>(command) ||
         std::holds_alternative<PublishCancelled>(command);
}

[[nodiscard]] inline MachineEvent event_for(const MachineEventKind kind) {
  using enum MachineEventKind;
  switch (kind) {
  case begin:
    return BeginTurn{.observed_at = at(0ms)};
  case text_delta:
    return ModelTextDelta{.text = "delta"};
  case completed:
    return ModelCompleted{};
  case attempt_failed:
    return AttemptFailed{
        .error = error(ErrorCategory::protocol),
        .observed_at = at(0ms),
    };
  case retry_wake:
    return RetryWake{.observed_at = at(100ms)};
  case cancel:
    return CancelTurn{};
  }
  return CancelTurn{};
}

} // namespace scry::detail::machine_test
