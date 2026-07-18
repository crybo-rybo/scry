#include "machine/turn_machine.hpp"

#include "core/retry.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace scry::detail {
namespace {

template <typename Command> [[nodiscard]] TransitionResult applied(Command command) {
  TransitionResult result{};
  result.commands.emplace_back(std::move(command));
  return result;
}

[[nodiscard]] std::chrono::milliseconds
bounded_elapsed(const RetryPolicy& policy) noexcept {
  return std::max(policy.max_elapsed, std::chrono::milliseconds{0});
}

} // namespace

TurnMachine::TurnMachine(TurnId turn_id, ModelRequest request, RetryPolicy retry_policy)
    : turn_id_(turn_id), request_(std::move(request)), retry_policy_(retry_policy) {}

TransitionResult TurnMachine::apply(MachineEvent event) {
  if (phase() == MachinePhase::terminal) {
    return {.status = TransitionStatus::ignored_terminal};
  }
  return std::visit(
      [this](auto&& value) { return on_event(std::forward<decltype(value)>(value)); },
      std::move(event));
}

MachinePhase TurnMachine::phase() const noexcept {
  constexpr std::array phases{
      MachinePhase::queued,     MachinePhase::awaiting_model, MachinePhase::streaming,
      MachinePhase::retry_wait, MachinePhase::terminal,
  };
  return phases[state_.index()];
}

std::uint32_t TurnMachine::attempt_count() const noexcept { return attempt_count_; }

std::optional<MachineTerminalKind> TurnMachine::terminal_kind() const noexcept {
  if (const auto* terminal = std::get_if<TerminalState>(&state_)) {
    return terminal->kind;
  }
  return std::nullopt;
}

TransitionResult TurnMachine::on_event(const BeginTurn event) {
  if (!std::holds_alternative<QueuedState>(state_)) {
    return illegal(MachineEventKind::begin,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  started_at_ = event.observed_at;
  latest_time_ = event.observed_at;
  return issue_attempt();
}

TransitionResult TurnMachine::on_event(ModelTextDelta event) {
  const auto* awaiting = std::get_if<AwaitingModelState>(&state_);
  const auto* streaming = std::get_if<StreamingState>(&state_);
  if (awaiting == nullptr && streaming == nullptr) {
    return illegal(MachineEventKind::text_delta,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  const auto attempt = awaiting != nullptr ? awaiting->attempt : streaming->attempt;
  state_.emplace<StreamingState>(attempt);
  return applied(PublishTextDelta{
      .turn_id = turn_id_,
      .text = std::move(event.text),
      .attempt = attempt,
  });
}

TransitionResult TurnMachine::on_event(const ModelSemanticOutput /*event*/) {
  const auto* awaiting = std::get_if<AwaitingModelState>(&state_);
  if (awaiting != nullptr) {
    const auto attempt = awaiting->attempt;
    state_.emplace<StreamingState>(attempt);
    return {};
  }
  if (std::holds_alternative<StreamingState>(state_)) {
    return {};
  }
  return illegal(MachineEventKind::semantic_output,
                 TransitionDiagnosticReason::event_not_allowed);
}

TransitionResult TurnMachine::on_event(ModelCompleted event) {
  if (phase() != MachinePhase::awaiting_model && phase() != MachinePhase::streaming) {
    return illegal(MachineEventKind::completed,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  state_.emplace<TerminalState>(MachineTerminalKind::completed);
  return applied(CommitCompletion{
      .turn_id = turn_id_,
      .response = std::move(event.response),
      .attempt_count = attempt_count_,
  });
}

TransitionResult TurnMachine::on_event(AttemptFailed event) {
  const auto current_phase = phase();
  if (current_phase != MachinePhase::awaiting_model &&
      current_phase != MachinePhase::streaming) {
    return illegal(MachineEventKind::attempt_failed,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  if (!latest_time_ || !started_at_) {
    return illegal(MachineEventKind::attempt_failed,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  if (event.observed_at < latest_time_.value()) {
    return illegal(MachineEventKind::attempt_failed,
                   TransitionDiagnosticReason::non_monotonic_time);
  }
  latest_time_ = event.observed_at;
  if (event.error.category == ErrorCategory::cancelled) {
    return on_event(CancelTurn{});
  }
  auto error = correlate(std::move(event.error));
  if (current_phase == MachinePhase::streaming ||
      !retry_is_allowed(error, event.observed_at)) {
    return finish_error(std::move(error));
  }

  const auto delay = retry_delay(retry_policy_, attempt_count_, event.retry_after,
                                 event.jitter_sample);
  const auto deadline = event.observed_at + delay;
  const auto elapsed_deadline = started_at_.value() + bounded_elapsed(retry_policy_);
  if (deadline > elapsed_deadline) {
    return finish_error(std::move(error));
  }
  state_.emplace<RetryWaitState>(deadline, std::move(error));
  return applied(ScheduleRetryWake{
      .turn_id = turn_id_,
      .deadline = deadline,
      .failed_attempt = attempt_count_,
  });
}

TransitionResult TurnMachine::on_event(const RetryWake event) {
  const auto* waiting = std::get_if<RetryWaitState>(&state_);
  if (waiting == nullptr) {
    return illegal(MachineEventKind::retry_wake,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  if (!started_at_) {
    return illegal(MachineEventKind::retry_wake,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  if (event.observed_at < waiting->deadline) {
    return illegal(MachineEventKind::retry_wake,
                   TransitionDiagnosticReason::wake_before_deadline);
  }
  const auto elapsed_deadline = started_at_.value() + bounded_elapsed(retry_policy_);
  if (event.observed_at > elapsed_deadline) {
    return finish_error(waiting->last_error);
  }
  latest_time_ = event.observed_at;
  return issue_attempt();
}

TransitionResult TurnMachine::on_event(const CancelTurn /*event*/) {
  state_.emplace<TerminalState>(MachineTerminalKind::cancelled);
  return applied(PublishCancelled{.turn_id = turn_id_});
}

TransitionResult TurnMachine::issue_attempt() {
  ++attempt_count_;
  state_.emplace<AwaitingModelState>(attempt_count_);
  return applied(IssueModelRequest{
      .turn_id = turn_id_,
      .request = request_,
      .attempt = attempt_count_,
  });
}

TransitionResult TurnMachine::finish_error(Error error) {
  state_.emplace<TerminalState>(MachineTerminalKind::failed);
  return applied(PublishError{.error = std::move(error)});
}

TransitionResult TurnMachine::illegal(const MachineEventKind event,
                                      const TransitionDiagnosticReason reason) const {
  return {
      .status = TransitionStatus::illegal_transition,
      .diagnostic =
          TransitionDiagnostic{
              .phase = phase(),
              .event = event,
              .reason = reason,
          },
  };
}

bool TurnMachine::retry_is_allowed(const Error& error,
                                   const MachineTimePoint observed_at) const noexcept {
  if (!started_at_ || !is_retryable(error.category) ||
      attempt_count_ >= retry_policy_.max_attempts) {
    return false;
  }
  const auto deadline = started_at_.value() + bounded_elapsed(retry_policy_);
  return observed_at <= deadline;
}

Error TurnMachine::correlate(Error error) const {
  error.retryable = is_retryable(error.category);
  error.turn_id = turn_id_;
  error.attempt = attempt_count_;
  return error;
}

} // namespace scry::detail
