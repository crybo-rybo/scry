#pragma once

#include "core/model.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <variant>
#include <vector>

namespace scry::detail {

using MachineTimePoint = std::chrono::steady_clock::time_point;

enum class MachinePhase : std::uint8_t {
  queued,
  awaiting_model,
  streaming,
  retry_wait,
  terminal,
};

enum class MachineTerminalKind : std::uint8_t {
  completed,
  failed,
  cancelled,
};

struct BeginTurn {
  MachineTimePoint observed_at{};
};

struct ModelTextDelta {
  std::string text{};
};

struct ModelCompleted {
  ModelResponse response{};
};

struct AttemptFailed {
  Error error{};
  MachineTimePoint observed_at{};
  std::optional<std::chrono::milliseconds> retry_after{};
  double jitter_sample{};
};

struct RetryWake {
  MachineTimePoint observed_at{};
};

struct CancelTurn {};

using MachineEvent = std::variant<BeginTurn, ModelTextDelta, ModelCompleted,
                                  AttemptFailed, RetryWake, CancelTurn>;

enum class MachineEventKind : std::uint8_t {
  begin,
  text_delta,
  completed,
  attempt_failed,
  retry_wake,
  cancel,
};

struct IssueModelRequest {
  TurnId turn_id{};
  ModelRequest request{};
  std::uint32_t attempt{};
};

struct PublishTextDelta {
  TurnId turn_id{};
  std::string text{};
  std::uint32_t attempt{};
};

struct ScheduleRetryWake {
  TurnId turn_id{};
  MachineTimePoint deadline{};
  std::uint32_t failed_attempt{};
};

// The driver forwards this terminal intent to the pump as one value. The pump
// owns the atomic Conversation commit and callback delivery.
struct CommitCompletion {
  TurnId turn_id{};
  ModelResponse response{};
  std::uint32_t attempt_count{};
};

struct PublishError {
  Error error{};
};

struct PublishCancelled {
  TurnId turn_id{};
};

using MachineCommand =
    std::variant<IssueModelRequest, PublishTextDelta, ScheduleRetryWake,
                 CommitCompletion, PublishError, PublishCancelled>;

enum class TransitionStatus : std::uint8_t {
  applied,
  ignored_terminal,
  illegal_transition,
};

enum class TransitionDiagnosticReason : std::uint8_t {
  event_not_allowed,
  non_monotonic_time,
  wake_before_deadline,
};

struct TransitionDiagnostic {
  MachinePhase phase{MachinePhase::queued};
  MachineEventKind event{MachineEventKind::begin};
  TransitionDiagnosticReason reason{TransitionDiagnosticReason::event_not_allowed};
};

struct TransitionResult {
  std::vector<MachineCommand> commands{};
  TransitionStatus status{TransitionStatus::applied};
  std::optional<TransitionDiagnostic> diagnostic{};
};

class TurnMachine {
public:
  TurnMachine(TurnId turn_id, ModelRequest request, RetryPolicy retry_policy);

  [[nodiscard]] TransitionResult apply(MachineEvent event);

  [[nodiscard]] MachinePhase phase() const noexcept;
  [[nodiscard]] std::uint32_t attempt_count() const noexcept;
  [[nodiscard]] std::optional<MachineTerminalKind> terminal_kind() const noexcept;

private:
  struct QueuedState {};

  struct AwaitingModelState {
    std::uint32_t attempt{};
  };

  struct StreamingState {
    std::uint32_t attempt{};
  };

  struct RetryWaitState {
    MachineTimePoint deadline{};
    Error last_error{};
  };

  struct TerminalState {
    MachineTerminalKind kind{MachineTerminalKind::failed};
  };

  using State = std::variant<QueuedState, AwaitingModelState, StreamingState,
                             RetryWaitState, TerminalState>;

  [[nodiscard]] TransitionResult on_event(BeginTurn event);
  [[nodiscard]] TransitionResult on_event(ModelTextDelta event);
  [[nodiscard]] TransitionResult on_event(ModelCompleted event);
  [[nodiscard]] TransitionResult on_event(AttemptFailed event);
  [[nodiscard]] TransitionResult on_event(RetryWake event);
  [[nodiscard]] TransitionResult on_event(CancelTurn event);

  [[nodiscard]] TransitionResult issue_attempt();
  [[nodiscard]] TransitionResult finish_error(Error error);
  [[nodiscard]] TransitionResult illegal(MachineEventKind event,
                                         TransitionDiagnosticReason reason) const;
  [[nodiscard]] bool retry_is_allowed(const Error& error,
                                      MachineTimePoint observed_at) const noexcept;
  [[nodiscard]] Error correlate(Error error) const;

  TurnId turn_id_{};
  ModelRequest request_{};
  RetryPolicy retry_policy_{};
  State state_{QueuedState{}};
  std::optional<MachineTimePoint> started_at_{};
  std::optional<MachineTimePoint> latest_time_{};
  std::uint32_t attempt_count_{};
};

} // namespace scry::detail
