#pragma once

#include "core/model.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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
  awaiting_tool,
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

struct ModelSemanticOutput {};

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

struct ToolResultReady {
  ToolResultBlock result{};
  MachineTimePoint observed_at{};
};

struct ToolExecutionFailed {
  Error error{};
};

struct CancelTurn {};

using MachineEvent = std::variant<BeginTurn, ModelTextDelta, ModelSemanticOutput,
                                  ModelCompleted, AttemptFailed, RetryWake,
                                  ToolResultReady, ToolExecutionFailed, CancelTurn>;

enum class MachineEventKind : std::uint8_t {
  begin,
  text_delta,
  semantic_output,
  completed,
  attempt_failed,
  retry_wake,
  tool_result_ready,
  tool_execution_failed,
  cancel,
};

// The request is a shared immutable snapshot rather than a per-attempt copy:
// retries and tool rounds reissue the same conversation, and the machine
// outlives every attempt that reads it. TurnMachine reseats the snapshot
// copy-on-write, so a snapshot handed to one attempt never observes the
// messages a later tool round appends.
struct IssueModelRequest {
  TurnId turn_id{};
  std::shared_ptr<const ModelRequest> request{};
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

struct PublishToolCall {
  TurnId turn_id{};
  ToolCallBlock call{};
  std::size_t remaining_exchange_bytes{std::numeric_limits<std::size_t>::max()};
};

// The driver forwards this terminal intent to the pump as one value. The pump
// owns the atomic Conversation commit and callback delivery.
struct CommitCompletion {
  TurnId turn_id{};
  std::vector<Message> exchange{};
  FinishReason finish_reason{FinishReason::unknown};
  Usage usage{};
  std::uint32_t attempt_count{};
  std::string provider_request_id{};
};

struct PublishError {
  Error error{};
};

struct PublishCancelled {
  TurnId turn_id{};
};

using MachineCommand =
    std::variant<IssueModelRequest, PublishTextDelta, ScheduleRetryWake,
                 PublishToolCall, CommitCompletion, PublishError, PublishCancelled>;

enum class TransitionStatus : std::uint8_t {
  applied,
  ignored_terminal,
  illegal_transition,
};

enum class TransitionDiagnosticReason : std::uint8_t {
  event_not_allowed,
  non_monotonic_time,
  wake_before_deadline,
  unknown_tool_call,
  duplicate_tool_result,
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

struct ToolLoopPolicy {
  std::uint32_t max_rounds{8};
  std::size_t max_argument_bytes{std::size_t{1024} * 1024};
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
};

class TurnMachine {
public:
  TurnMachine(TurnId turn_id, ModelRequest request, RetryPolicy retry_policy,
              ToolLoopPolicy tool_policy = {});

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

  struct PendingToolCall {
    ToolCallBlock call{};
    std::optional<ToolResultBlock> result{};
  };

  struct AwaitingToolState {
    Message assistant{};
    std::vector<PendingToolCall> calls{};
    std::size_t results_received{};
    std::string provider_request_id{};
  };

  struct TerminalState {
    MachineTerminalKind kind{MachineTerminalKind::failed};
  };

  using State = std::variant<QueuedState, AwaitingModelState, StreamingState,
                             RetryWaitState, AwaitingToolState, TerminalState>;

  [[nodiscard]] TransitionResult on_event(BeginTurn event);
  [[nodiscard]] TransitionResult on_event(ModelTextDelta event);
  [[nodiscard]] TransitionResult on_event(ModelSemanticOutput event);
  [[nodiscard]] TransitionResult on_event(ModelCompleted event);
  [[nodiscard]] TransitionResult on_event(AttemptFailed event);
  [[nodiscard]] TransitionResult on_event(RetryWake event);
  [[nodiscard]] TransitionResult on_event(ToolResultReady event);
  [[nodiscard]] TransitionResult on_event(ToolExecutionFailed event);
  [[nodiscard]] TransitionResult on_event(CancelTurn event);

  [[nodiscard]] TransitionResult start_request(MachineTimePoint observed_at);
  [[nodiscard]] TransitionResult issue_attempt();
  [[nodiscard]] TransitionResult begin_tool_round(ModelResponse response,
                                                  std::vector<ToolCallBlock> calls);
  [[nodiscard]] TransitionResult complete_turn(ModelResponse response);
  [[nodiscard]] TransitionResult finish_error(Error error);
  [[nodiscard]] TransitionResult fail(ErrorCategory category, std::string message);
  [[nodiscard]] TransitionResult fail_response(ErrorCategory category,
                                               std::string message,
                                               std::string provider_request_id);
  [[nodiscard]] TransitionResult illegal(MachineEventKind event,
                                         TransitionDiagnosticReason reason) const;
  [[nodiscard]] bool retry_is_allowed(const Error& error,
                                      MachineTimePoint observed_at) const noexcept;
  [[nodiscard]] Result<std::vector<ToolCallBlock>>
  validate_response(const ModelResponse& response) const;
  [[nodiscard]] ModelRequest& mutable_request();
  [[nodiscard]] bool usage_would_overflow(const Usage& usage) const noexcept;
  [[nodiscard]] bool reserve_exchange_bytes(std::size_t bytes) noexcept;
  [[nodiscard]] std::size_t remaining_exchange_bytes() const noexcept;
  void accumulate_usage(const Usage& usage) noexcept;
  [[nodiscard]] Error correlate(Error error) const;

  TurnId turn_id_{};
  std::shared_ptr<ModelRequest> request_{};
  RetryPolicy retry_policy_{};
  ToolLoopPolicy tool_policy_{};
  State state_{QueuedState{}};
  std::vector<Message> exchange_{};
  std::optional<MachineTimePoint> request_started_at_{};
  std::optional<MachineTimePoint> latest_time_{};
  std::uint32_t attempt_count_{};
  std::uint32_t request_attempt_count_{};
  std::uint32_t tool_round_count_{};
  std::size_t exchange_payload_bytes_{};
  Usage usage_{};
  std::vector<std::string> dispatched_tool_ids_{};
};

} // namespace scry::detail
