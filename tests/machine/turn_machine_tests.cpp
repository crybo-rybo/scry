#include "machine_test_support.hpp"

#include <array>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;
using namespace scry::detail::machine_test;

static_assert(std::is_move_constructible_v<MachineEvent>);
static_assert(std::is_move_constructible_v<MachineCommand>);

namespace {

// Applies each event to a fresh machine that setup puts in the phase under test.
template <typename Setup>
void check_illegal_transitions(std::vector<MachineEvent> events, const Setup setup) {
  for (auto& event : events) {
    INFO("event index " << event.index());
    auto machine = make_machine();
    setup(machine);
    const auto original_phase = machine.phase();
    const auto original_attempts = machine.attempt_count();
    const auto result = machine.apply(std::move(event));
    CHECK(result.status == TransitionStatus::event_not_allowed);
    CHECK(result.commands.empty());
    CHECK(machine.phase() == original_phase);
    CHECK(machine.attempt_count() == original_attempts);
  }
}

} // namespace

TEST_CASE("queued turn issues its first model attempt") {
  auto machine = make_machine();

  CHECK(machine.phase() == MachinePhase::queued);
  CHECK(machine.attempt_count() == 0);

  const auto result = machine.apply(BeginTurn{.observed_at = at(250ms)});
  const auto& command = only_command<IssueModelRequest>(result);

  CHECK(command.turn_id == turn_id);
  CHECK(command.request->system_prompt == "Be concise.");
  CHECK(machine.phase() == MachinePhase::awaiting_model);
  CHECK(machine.attempt_count() == 1);
}

TEST_CASE("non-streaming completion emits one transactional commit intent") {
  auto machine = make_machine();
  begin(machine);

  ModelResponse response{
      .content = {TextBlock{.text = "answer"}},
      .finish_reason = FinishReason::completed,
      .usage = {.input_tokens = 4, .output_tokens = 2},
      .provider_request_id = "provider-id",
  };
  const auto result = machine.apply(ModelCompleted{.response = std::move(response)});
  const auto& command = only_command<CommitCompletion>(result);

  CHECK(command.turn_id == turn_id);
  CHECK(command.attempt_count == 1);
  CHECK(command.finish_reason == FinishReason::completed);
  CHECK(command.tool_round_count == 0);
  CHECK(command.tool_call_count == 0);
  CHECK(command.provider_request_id == "provider-id");
  CHECK(command.usage.input_tokens == 4);
  // The transcript is the request's own message list, so it opens with the user
  // message the turn was built from and ends with the assistant reply.
  REQUIRE(command.transcript.size() == 2);
  CHECK(command.transcript.front().role == Role::user);
  CHECK(command.transcript.back().role == Role::assistant);
  REQUIRE(command.transcript.back().content.size() == 1);
  CHECK(std::get<TextBlock>(command.transcript.back().content.front()).text ==
        "answer");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("empty text blocks are dropped before the exchange is committed") {
  auto machine = make_machine();
  begin(machine);

  ModelResponse response{
      .content = {TextBlock{.text = ""}, TextBlock{.text = "hi"}},
      .finish_reason = FinishReason::completed,
  };
  const auto result = machine.apply(ModelCompleted{.response = std::move(response)});
  const auto& command = only_command<CommitCompletion>(result);

  // The transcript opens with the turn's user message.
  REQUIRE(command.transcript.size() == 2);
  const auto& assistant = command.transcript.back();
  CHECK(assistant.role == Role::assistant);
  REQUIRE(assistant.content.size() == 1);
  CHECK(std::get<TextBlock>(assistant.content.front()).text == "hi");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("a response carrying no content fails the turn instead of committing") {
  auto machine = make_machine();
  begin(machine);

  std::vector<ContentBlock> content;
  SECTION("one empty text block") { content.push_back(TextBlock{.text = ""}); }
  SECTION("no blocks at all") {}

  ModelResponse response{
      .content = std::move(content),
      .finish_reason = FinishReason::completed,
  };
  const auto result = machine.apply(ModelCompleted{.response = std::move(response)});
  const auto& error = only_command<PublishError>(result).error;

  CHECK(error.category == scry::ErrorCategory::protocol);
  CHECK(error.message == "model response contained no content");
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("a tool round starts without the response's empty text block") {
  auto machine = make_machine();
  begin(machine);

  const auto published = machine.apply(ModelCompleted{
      .response = tool_response({TextBlock{.text = ""}, tool_call("call-1", "lookup")}),
  });
  CHECK(only_command<PublishToolCall>(published).call.id == "call-1");

  const auto issued = machine.apply(ToolResultReady{
      .result =
          {
              .tool_call_id = "call-1",
              .result = scry::Json{.text = R"({"ok":true})"},
          },
      .observed_at = at(1ms),
  });
  only_command<IssueModelRequest>(issued);

  const auto completed = machine.apply(ModelCompleted{
      .response =
          {
              .content = {TextBlock{.text = "done"}},
              .finish_reason = FinishReason::completed,
          },
  });
  const auto& commit = only_command<CommitCompletion>(completed);

  // user, assistant tool round, tool results, final assistant.
  REQUIRE(commit.transcript.size() == 4);
  const auto& assistant = commit.transcript[1];
  CHECK(assistant.role == Role::assistant);
  REQUIRE(assistant.content.size() == 1);
  CHECK(std::get<ToolCallBlock>(assistant.content.front()).id == "call-1");
}

TEST_CASE("terminal transitions release the model request snapshot") {
  const auto history = std::make_shared<std::vector<Message>>();
  auto model_request = request();
  model_request.history = history;
  TurnMachine machine{turn_id, std::move(model_request), retry_policy(), tool_policy()};
  auto issued = machine.apply(BeginTurn{});
  issued.commands.clear();
  REQUIRE(history.use_count() == 2);

  SECTION("completion") {
    const auto completed = machine.apply(ModelCompleted{.response = text_response()});
    only_command<CommitCompletion>(completed);
    CHECK(history.use_count() == 1);
  }

  SECTION("failure") {
    const auto failed = machine.apply(AttemptFailed{
        .error = error(scry::ErrorCategory::protocol),
    });
    only_command<PublishError>(failed);
    CHECK(history.use_count() == 1);
  }

  SECTION("cancellation") {
    const auto cancelled = machine.apply(CancelTurn{});
    only_command<PublishCancelled>(cancelled);
    CHECK(history.use_count() == 1);
  }
}

TEST_CASE("text deltas enter streaming and preserve attempt correlation") {
  auto machine = make_machine();
  begin(machine);

  const auto first = machine.apply(ModelTextDelta{.text = "hel"});
  const auto& first_command = only_command<PublishTextDelta>(first);
  CHECK(first_command.text == "hel");
  CHECK(first_command.attempt == 1);
  CHECK(machine.phase() == MachinePhase::streaming);

  const auto second = machine.apply(ModelTextDelta{.text = "lo"});
  const auto& second_command = only_command<PublishTextDelta>(second);
  CHECK(second_command.text == "lo");
  CHECK(second_command.attempt == 1);

  const auto completed = machine.apply(ModelCompleted{.response = text_response()});
  only_command<CommitCompletion>(completed);
}

TEST_CASE("transient pre-output failure schedules a deterministic retry") {
  auto machine = make_machine();
  begin(machine, 1s);

  const auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::rate_limit),
      .observed_at = at(1100ms),
      .retry_after = 400ms,
      .jitter_sample = 1.0,
  });
  const auto& schedule = only_command<ScheduleRetryWake>(failed);

  CHECK(schedule.deadline == at(1500ms));
  CHECK(machine.phase() == MachinePhase::retry_wait);
  CHECK(machine.attempt_count() == 1);

  const auto wake = machine.apply(RetryWake{.observed_at = at(1500ms)});
  only_command<IssueModelRequest>(wake);
  CHECK(machine.attempt_count() == 2);
  CHECK(machine.phase() == MachinePhase::awaiting_model);
}

TEST_CASE("retry deadlines saturate at the steady clock maximum") {
  auto policy = retry_policy();
  policy.initial_backoff = 10ms;
  policy.max_backoff = 10ms;
  policy.max_elapsed = 100ms;
  auto machine = make_machine(policy);
  const auto started = MachineTimePoint::max() - 5ms;
  const auto begun = machine.apply(BeginTurn{.observed_at = started});
  only_command<IssueModelRequest>(begun);

  const auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = started,
  });
  const auto& schedule = only_command<ScheduleRetryWake>(failed);
  CHECK(schedule.deadline == MachineTimePoint::max());

  const auto wake = machine.apply(RetryWake{.observed_at = schedule.deadline});
  only_command<IssueModelRequest>(wake);
  CHECK(machine.attempt_count() == 2);
}

TEST_CASE("retry durations larger than the steady clock range saturate") {
  using Milliseconds = std::chrono::milliseconds;
  constexpr auto maximum = Milliseconds{std::numeric_limits<Milliseconds::rep>::max()};
  auto policy = retry_policy();
  policy.initial_backoff = maximum;
  policy.max_backoff = maximum;
  policy.max_elapsed = maximum;
  auto machine = make_machine(policy);
  begin(machine);

  const auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = MachineTimePoint{},
  });
  const auto& schedule = only_command<ScheduleRetryWake>(failed);
  CHECK(schedule.deadline == MachineTimePoint::max());

  const auto wake = machine.apply(RetryWake{.observed_at = schedule.deadline});
  only_command<IssueModelRequest>(wake);
  CHECK(machine.attempt_count() == 2);
}

TEST_CASE("machine integrates exponential backoff and injected jitter") {
  auto policy = retry_policy();
  policy.jitter_ratio = 0.5;
  auto machine = make_machine(policy);
  begin(machine);

  const auto first_failure = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(0ms),
      .jitter_sample = 1.0,
  });
  const auto& first_schedule = only_command<ScheduleRetryWake>(first_failure);
  CHECK(first_schedule.deadline == at(150ms));

  const auto first_wake = machine.apply(RetryWake{.observed_at = at(150ms)});
  only_command<IssueModelRequest>(first_wake);
  const auto second_failure = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(200ms),
      .jitter_sample = -1.0,
  });
  const auto& second_schedule = only_command<ScheduleRetryWake>(second_failure);
  CHECK(second_schedule.deadline == at(300ms));
}

TEST_CASE("attempt cap publishes the last retryable error") {
  auto policy = retry_policy();
  policy.max_attempts = 2;
  auto machine = make_machine(policy);
  begin(machine);

  auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(0ms),
  });
  const auto deadline = only_command<ScheduleRetryWake>(failed).deadline;
  auto wake = machine.apply(RetryWake{.observed_at = deadline});
  only_command<IssueModelRequest>(wake);

  failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network, "second failure"),
      .observed_at = at(200ms),
  });
  const auto& terminal = only_command<PublishError>(failed);
  CHECK(terminal.error.message == "second failure");
  CHECK(terminal.error.retryable);
  CHECK(terminal.error.attempt == 2);
  CHECK(terminal.error.turn_id == turn_id);
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("elapsed retry cap rejects waits beyond the deadline") {
  auto policy = retry_policy();
  policy.initial_backoff = 200ms;
  policy.max_elapsed = 1s;

  SECTION("failure arrives after the elapsed deadline") {
    auto machine = make_machine(policy);
    begin(machine);
    const auto failed = machine.apply(AttemptFailed{
        .error = error(scry::ErrorCategory::network),
        .observed_at = at(1001ms),
    });
    only_command<PublishError>(failed);
  }

  SECTION("computed wake would cross the elapsed deadline") {
    auto machine = make_machine(policy);
    begin(machine);
    const auto failed = machine.apply(AttemptFailed{
        .error = error(scry::ErrorCategory::network),
        .observed_at = at(900ms),
    });
    only_command<PublishError>(failed);
  }

  SECTION("delayed wake does not start an attempt after the deadline") {
    auto machine = make_machine(policy);
    begin(machine);
    const auto failed = machine.apply(AttemptFailed{
        .error = error(scry::ErrorCategory::network),
        .observed_at = at(0ms),
    });
    only_command<ScheduleRetryWake>(failed);
    const auto wake = machine.apply(RetryWake{.observed_at = at(1001ms)});
    only_command<PublishError>(wake);
  }
}

TEST_CASE("semantic output prevents automatic retry") {
  auto machine = make_machine();
  begin(machine);
  const auto observed = machine.apply(ModelSemanticOutput{});
  CHECK(observed.status == TransitionStatus::applied);
  CHECK(observed.commands.empty());
  CHECK(machine.phase() == MachinePhase::streaming);

  const auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::network),
      .observed_at = at(1ms),
  });
  const auto& command = only_command<PublishError>(failed);

  CHECK(command.error.retryable);
  CHECK(command.error.attempt == 1);
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("repeated semantic output remains in streaming") {
  auto machine = make_machine();
  enter_streaming(machine);

  const auto observed = machine.apply(ModelSemanticOutput{});
  CHECK(observed.status == TransitionStatus::applied);
  CHECK(observed.commands.empty());
  CHECK(machine.phase() == MachinePhase::streaming);
}

TEST_CASE("non-retryable categories terminate without a wake") {
  using scry::ErrorCategory;
  const std::array categories{
      ErrorCategory::invalid_config,   ErrorCategory::invalid_state,
      ErrorCategory::invalid_argument, ErrorCategory::busy,
      ErrorCategory::authentication,   ErrorCategory::protocol,
      ErrorCategory::resource_limit,   ErrorCategory::tool,
      ErrorCategory::max_tool_rounds,
  };

  for (const auto category : categories) {
    auto machine = make_machine();
    begin(machine);
    const auto failed = machine.apply(AttemptFailed{
        .error = error(category),
        .observed_at = at(1ms),
    });
    const auto& command = only_command<PublishError>(failed);
    CHECK(command.error.category == category);
    CHECK_FALSE(command.error.retryable);
    CHECK(machine.phase() == MachinePhase::terminal);
  }
}

TEST_CASE("transport cancellation maps to the cancelled terminal channel") {
  auto machine = make_machine();
  begin(machine);

  const auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::cancelled),
      .observed_at = at(1ms),
  });

  CHECK(only_command<PublishCancelled>(failed).turn_id == turn_id);
  CHECK(machine.phase() == MachinePhase::terminal);
}

TEST_CASE("error commands carry stable turn and attempt correlation") {
  auto machine = make_machine();
  begin(machine);

  const auto failed = machine.apply(AttemptFailed{
      .error = error(scry::ErrorCategory::authentication, "denied"),
      .observed_at = at(25ms),
  });
  const auto& output = only_command<PublishError>(failed).error;

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
    const auto result = machine.apply(CancelTurn{});
    CHECK(only_command<PublishCancelled>(result).turn_id == turn_id);
    CHECK(machine.attempt_count() == 0);
  }

  SECTION("awaiting model") {
    auto machine = make_machine();
    begin(machine);
    const auto result = machine.apply(CancelTurn{});
    only_command<PublishCancelled>(result);
  }

  SECTION("streaming") {
    auto machine = make_machine();
    enter_streaming(machine);
    const auto result = machine.apply(CancelTurn{});
    only_command<PublishCancelled>(result);
  }

  SECTION("retry wait") {
    auto machine = make_machine();
    enter_retry_wait(machine);
    const auto result = machine.apply(CancelTurn{});
    only_command<PublishCancelled>(result);
  }

  SECTION("awaiting tool") {
    auto machine = make_machine();
    enter_awaiting_tool(machine);
    const auto result = machine.apply(CancelTurn{});
    only_command<PublishCancelled>(result);
  }
}

TEST_CASE("request phases reject illegal transitions without mutation") {
  SECTION("queued accepts only begin or cancel") {
    check_illegal_transitions(events_except<BeginTurn, CancelTurn>(),
                              [](TurnMachine&) {});
  }

  SECTION("awaiting model rejects begin, wake, and tool result") {
    check_illegal_transitions(
        events_except<ModelTextDelta, ModelSemanticOutput, ModelCompleted,
                      AttemptFailed, CancelTurn>(),
        [](TurnMachine& machine) { begin(machine); });
  }

  SECTION("streaming rejects begin, wake, and tool result") {
    check_illegal_transitions(
        events_except<ModelTextDelta, ModelSemanticOutput, ModelCompleted,
                      AttemptFailed, CancelTurn>(),
        enter_streaming);
  }
}

TEST_CASE("waiting phases reject illegal transitions without mutation") {
  SECTION("retry wait accepts only wake or cancel") {
    check_illegal_transitions(events_except<RetryWake, CancelTurn>(), enter_retry_wait);
  }

  SECTION("awaiting tool accepts only a tool result or cancel") {
    check_illegal_transitions(
        events_except<ToolResultReady, ToolExecutionFailed, CancelTurn>(),
        enter_awaiting_tool);
  }
}

TEST_CASE("retry wakes cannot arrive early") {
  auto machine = make_machine();
  enter_retry_wait(machine);
  const auto early = machine.apply(RetryWake{.observed_at = at(99ms)});
  CHECK(early.status == TransitionStatus::wake_before_deadline);
  CHECK(early.commands.empty());
  CHECK(machine.phase() == MachinePhase::retry_wait);

  const auto due = machine.apply(RetryWake{.observed_at = at(100ms)});
  only_command<IssueModelRequest>(due);
  CHECK(machine.attempt_count() == 2);
}

// Only the first terminal event counts; every later event, terminal or not, is
// ignored.
TEST_CASE("terminal state ignores every later event") {
  std::vector<MachineEvent> terminal_events{
      ModelCompleted{.response = text_response()},
      AttemptFailed{
          .error = error(scry::ErrorCategory::protocol),
          .observed_at = at(1ms),
      },
      CancelTurn{},
  };
  for (auto& terminal_event : terminal_events) {
    INFO("terminal event index " << terminal_event.index());
    auto machine = make_machine();
    begin(machine);
    const auto ended = machine.apply(std::move(terminal_event));
    REQUIRE(ended.commands.size() == 1);
    CHECK(is_terminal_command(ended.commands.front()));
    CHECK(machine.phase() == MachinePhase::terminal);

    for (auto& event : sample_events()) {
      const auto ignored = machine.apply(std::move(event));
      CHECK(ignored.status == TransitionStatus::ignored_terminal);
      CHECK(ignored.commands.empty());
    }
  }
}
