#include "machine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::detail::machine_test;

static_assert(std::is_move_constructible_v<scry::detail::MachineEvent>);
static_assert(std::is_move_constructible_v<scry::detail::MachineCommand>);

TEST_CASE("queued turn issues its first model attempt") {
  auto machine = make_machine();

  CHECK(machine.phase() == scry::detail::MachinePhase::queued);
  CHECK(machine.attempt_count() == 0);
  CHECK_FALSE(machine.terminal_kind().has_value());

  const auto result = machine.apply(scry::detail::BeginTurn{.observed_at = at(250ms)});
  const auto& command = only_command<scry::detail::IssueModelRequest>(result);

  CHECK(command.turn_id == turn_id);
  CHECK(command.attempt == 1);
  CHECK(command.request.model == "test-model");
  CHECK(command.request.system_prompt == "Be concise.");
  CHECK(machine.phase() == scry::detail::MachinePhase::awaiting_model);
  CHECK(machine.attempt_count() == 1);
}

TEST_CASE("non-streaming completion emits one transactional commit intent") {
  auto machine = make_machine();
  begin(machine);

  scry::detail::ModelResponse response{
      .content = {scry::detail::TextBlock{.text = "answer"}},
      .finish_reason = scry::detail::FinishReason::completed,
      .usage = {.input_tokens = 4, .output_tokens = 2},
      .provider_request_id = "provider-id",
  };
  const auto result =
      machine.apply(scry::detail::ModelCompleted{.response = std::move(response)});
  const auto& command = only_command<scry::detail::CommitCompletion>(result);

  CHECK(command.turn_id == turn_id);
  CHECK(command.attempt_count == 1);
  CHECK(command.response.provider_request_id == "provider-id");
  CHECK(command.response.usage.input_tokens == 4);
  CHECK(machine.phase() == scry::detail::MachinePhase::terminal);
  CHECK(machine.terminal_kind() == scry::detail::MachineTerminalKind::completed);
}

TEST_CASE("text deltas enter streaming and preserve attempt correlation") {
  auto machine = make_machine();
  begin(machine);

  const auto first = machine.apply(scry::detail::ModelTextDelta{.text = "hel"});
  const auto& first_command = only_command<scry::detail::PublishTextDelta>(first);
  CHECK(first_command.text == "hel");
  CHECK(first_command.attempt == 1);
  CHECK(machine.phase() == scry::detail::MachinePhase::streaming);

  const auto second = machine.apply(scry::detail::ModelTextDelta{.text = "lo"});
  const auto& second_command = only_command<scry::detail::PublishTextDelta>(second);
  CHECK(second_command.text == "lo");
  CHECK(second_command.attempt == 1);

  const auto completed = machine.apply(scry::detail::ModelCompleted{});
  static_cast<void>(only_command<scry::detail::CommitCompletion>(completed));
}

TEST_CASE("transient pre-output failure schedules a deterministic retry") {
  auto machine = make_machine();
  begin(machine, 1s);

  const auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::rate_limit),
      .observed_at = at(1100ms),
      .retry_after = 400ms,
      .jitter_sample = 1.0,
  });
  const auto& schedule = only_command<scry::detail::ScheduleRetryWake>(failed);

  CHECK(schedule.turn_id == turn_id);
  CHECK(schedule.failed_attempt == 1);
  CHECK(schedule.deadline == at(1500ms));
  CHECK(machine.phase() == scry::detail::MachinePhase::retry_wait);

  const auto wake = machine.apply(scry::detail::RetryWake{.observed_at = at(1500ms)});
  const auto& issue = only_command<scry::detail::IssueModelRequest>(wake);
  CHECK(issue.attempt == 2);
  CHECK(machine.phase() == scry::detail::MachinePhase::awaiting_model);
}

TEST_CASE("machine integrates exponential backoff and injected jitter") {
  auto policy = retry_policy();
  policy.jitter_ratio = 0.5;
  auto machine = make_machine(policy);
  begin(machine);

  const auto first_failure = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(0ms),
      .jitter_sample = 1.0,
  });
  const auto& first_schedule =
      only_command<scry::detail::ScheduleRetryWake>(first_failure);
  CHECK(first_schedule.deadline == at(150ms));

  const auto first_wake =
      machine.apply(scry::detail::RetryWake{.observed_at = at(150ms)});
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(first_wake));
  const auto second_failure = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(200ms),
      .jitter_sample = -1.0,
  });
  const auto& second_schedule =
      only_command<scry::detail::ScheduleRetryWake>(second_failure);
  CHECK(second_schedule.deadline == at(300ms));
}

TEST_CASE("attempt cap publishes the last retryable error") {
  auto policy = retry_policy();
  policy.max_attempts = 2;
  auto machine = make_machine(policy);
  begin(machine);

  auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(0ms),
  });
  const auto deadline = only_command<scry::detail::ScheduleRetryWake>(failed).deadline;
  auto wake = machine.apply(scry::detail::RetryWake{.observed_at = deadline});
  static_cast<void>(only_command<scry::detail::IssueModelRequest>(wake));

  failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network, "second failure"),
      .observed_at = at(200ms),
  });
  const auto& terminal = only_command<scry::detail::PublishError>(failed);
  CHECK(terminal.error.message == "second failure");
  CHECK(terminal.error.retryable);
  CHECK(terminal.error.attempt == 2);
  CHECK(terminal.error.turn_id == turn_id);
  CHECK(machine.terminal_kind() == scry::detail::MachineTerminalKind::failed);
}

TEST_CASE("elapsed retry cap rejects waits beyond the deadline") {
  auto policy = retry_policy();
  policy.initial_backoff = 200ms;
  policy.max_elapsed = 1s;

  SECTION("failure arrives after the elapsed deadline") {
    auto machine = make_machine(policy);
    begin(machine);
    const auto failed = machine.apply(scry::detail::AttemptFailed{
        .error = error(scry::ErrorCategory::network),
        .observed_at = at(1001ms),
    });
    static_cast<void>(only_command<scry::detail::PublishError>(failed));
  }

  SECTION("computed wake would cross the elapsed deadline") {
    auto machine = make_machine(policy);
    begin(machine);
    const auto failed = machine.apply(scry::detail::AttemptFailed{
        .error = error(scry::ErrorCategory::network),
        .observed_at = at(900ms),
    });
    static_cast<void>(only_command<scry::detail::PublishError>(failed));
  }

  SECTION("delayed wake does not start an attempt after the deadline") {
    auto machine = make_machine(policy);
    begin(machine);
    const auto failed = machine.apply(scry::detail::AttemptFailed{
        .error = error(scry::ErrorCategory::network),
        .observed_at = at(0ms),
    });
    static_cast<void>(only_command<scry::detail::ScheduleRetryWake>(failed));
    const auto wake = machine.apply(scry::detail::RetryWake{.observed_at = at(1001ms)});
    static_cast<void>(only_command<scry::detail::PublishError>(wake));
  }
}

TEST_CASE("semantic output prevents automatic retry") {
  auto machine = make_machine();
  enter_streaming(machine);

  const auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(1ms),
  });
  const auto& command = only_command<scry::detail::PublishError>(failed);

  CHECK(command.error.retryable);
  CHECK(command.error.attempt == 1);
  CHECK(machine.phase() == scry::detail::MachinePhase::terminal);
}

TEST_CASE("non-retryable categories terminate without a wake") {
  using scry::ErrorCategory;
  const std::array categories{
      ErrorCategory::invalid_config, ErrorCategory::invalid_state,
      ErrorCategory::busy,           ErrorCategory::authentication,
      ErrorCategory::protocol,       ErrorCategory::resource_limit,
      ErrorCategory::tool,           ErrorCategory::max_tool_rounds,
  };

  for (const auto category : categories) {
    auto machine = make_machine();
    begin(machine);
    const auto failed = machine.apply(scry::detail::AttemptFailed{
        .error = error(category),
        .observed_at = at(1ms),
    });
    const auto& command = only_command<scry::detail::PublishError>(failed);
    CHECK(command.error.category == category);
    CHECK_FALSE(command.error.retryable);
  }
}

TEST_CASE("transport cancellation maps to the cancelled terminal channel") {
  auto machine = make_machine();
  begin(machine);

  const auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::cancelled),
      .observed_at = at(1ms),
  });

  CHECK(only_command<scry::detail::PublishCancelled>(failed).turn_id == turn_id);
  CHECK(machine.terminal_kind() == scry::detail::MachineTerminalKind::cancelled);
}

TEST_CASE("error commands carry stable turn and attempt correlation") {
  auto machine = make_machine();
  begin(machine);

  const auto failed = machine.apply(scry::detail::AttemptFailed{
      .error = error(scry::ErrorCategory::authentication, "denied"),
      .observed_at = at(25ms),
  });
  const auto& output = only_command<scry::detail::PublishError>(failed).error;

  CHECK(output.category == scry::ErrorCategory::authentication);
  CHECK(output.message == "denied");
  CHECK(output.provider_detail == "sanitized detail");
  CHECK(output.provider_request_id == "request-123");
  CHECK(output.turn_id == turn_id);
  CHECK(output.attempt == 1);
}

TEST_CASE("cancellation terminates every live phase without I/O") {
  SECTION("queued") {
    auto machine = make_machine();
    const auto result = machine.apply(scry::detail::CancelTurn{});
    CHECK(only_command<scry::detail::PublishCancelled>(result).turn_id == turn_id);
    CHECK(machine.attempt_count() == 0);
  }

  SECTION("awaiting model") {
    auto machine = make_machine();
    begin(machine);
    const auto result = machine.apply(scry::detail::CancelTurn{});
    static_cast<void>(only_command<scry::detail::PublishCancelled>(result));
  }

  SECTION("streaming") {
    auto machine = make_machine();
    enter_streaming(machine);
    const auto result = machine.apply(scry::detail::CancelTurn{});
    static_cast<void>(only_command<scry::detail::PublishCancelled>(result));
  }

  SECTION("retry wait") {
    auto machine = make_machine();
    enter_retry_wait(machine);
    const auto result = machine.apply(scry::detail::CancelTurn{});
    static_cast<void>(only_command<scry::detail::PublishCancelled>(result));
  }
}

TEST_CASE("illegal transitions are diagnosed without mutating the machine") {
  using scry::detail::MachineEventKind;
  using scry::detail::MachinePhase;

  const auto check_illegal = [](scry::detail::TurnMachine& machine,
                                const MachineEventKind kind) {
    const auto original_phase = machine.phase();
    const auto original_attempts = machine.attempt_count();
    const auto result = machine.apply(event_for(kind));
    REQUIRE(result.status == scry::detail::TransitionStatus::illegal_transition);
    REQUIRE(result.commands.empty());
    REQUIRE(result.diagnostic.has_value());
    CHECK(result.diagnostic->phase == original_phase);
    CHECK(result.diagnostic->event == kind);
    CHECK(result.diagnostic->reason ==
          scry::detail::TransitionDiagnosticReason::event_not_allowed);
    CHECK(machine.phase() == original_phase);
    CHECK(machine.attempt_count() == original_attempts);
  };

  SECTION("queued accepts only begin or cancel") {
    for (const auto kind :
         {MachineEventKind::text_delta, MachineEventKind::completed,
          MachineEventKind::attempt_failed, MachineEventKind::retry_wake}) {
      auto machine = make_machine();
      check_illegal(machine, kind);
    }
  }

  SECTION("awaiting model rejects begin and wake") {
    for (const auto kind : {MachineEventKind::begin, MachineEventKind::retry_wake}) {
      auto machine = make_machine();
      begin(machine);
      check_illegal(machine, kind);
    }
  }

  SECTION("streaming rejects begin and wake") {
    for (const auto kind : {MachineEventKind::begin, MachineEventKind::retry_wake}) {
      auto machine = make_machine();
      enter_streaming(machine);
      check_illegal(machine, kind);
    }
  }

  SECTION("retry wait accepts only wake or cancel") {
    for (const auto kind :
         {MachineEventKind::begin, MachineEventKind::text_delta,
          MachineEventKind::completed, MachineEventKind::attempt_failed}) {
      auto machine = make_machine();
      enter_retry_wait(machine);
      check_illegal(machine, kind);
    }
  }
}

TEST_CASE("injected time is monotonic and retry wakes cannot arrive early") {
  SECTION("failure timestamp predates the last injected time") {
    auto machine = make_machine();
    begin(machine, 10ms);
    const auto result = machine.apply(scry::detail::AttemptFailed{
        .error = error(scry::ErrorCategory::network),
        .observed_at = at(9ms),
    });
    REQUIRE(result.diagnostic.has_value());
    CHECK(result.diagnostic->reason ==
          scry::detail::TransitionDiagnosticReason::non_monotonic_time);
    CHECK(machine.phase() == scry::detail::MachinePhase::awaiting_model);
  }

  SECTION("early wake retains retry state and deadline") {
    auto machine = make_machine();
    enter_retry_wait(machine);
    const auto result = machine.apply(scry::detail::RetryWake{.observed_at = at(99ms)});
    REQUIRE(result.diagnostic.has_value());
    CHECK(result.diagnostic->reason ==
          scry::detail::TransitionDiagnosticReason::wake_before_deadline);
    CHECK(machine.phase() == scry::detail::MachinePhase::retry_wait);

    const auto due = machine.apply(scry::detail::RetryWake{.observed_at = at(100ms)});
    CHECK(only_command<scry::detail::IssueModelRequest>(due).attempt == 2);
  }
}

TEST_CASE("terminal state is idempotent across event orderings") {
  for (std::uint32_t seed = 0; seed < 64; ++seed) {
    auto machine = make_machine();
    begin(machine);
    std::vector<scry::detail::MachineEvent> terminal_events{
        scry::detail::ModelCompleted{},
        scry::detail::AttemptFailed{
            .error = error(scry::ErrorCategory::protocol),
            .observed_at = at(1ms),
        },
        scry::detail::CancelTurn{},
    };
    std::mt19937 generator{seed};
    std::shuffle(terminal_events.begin(), terminal_events.end(), generator);

    std::size_t terminal_commands{};
    for (auto& event : terminal_events) {
      const auto result = machine.apply(std::move(event));
      terminal_commands += static_cast<std::size_t>(
          std::ranges::count_if(result.commands, is_terminal_command));
    }

    CHECK(terminal_commands == 1);
    CHECK(machine.phase() == scry::detail::MachinePhase::terminal);
  }
}
