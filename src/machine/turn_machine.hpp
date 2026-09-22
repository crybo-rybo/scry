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

// Tells the worker to send one model request. The request is shared rather than
// copied, because retries and tool rounds resend the same conversation; the
// machine copies it before adding a tool round, so the snapshot an attempt is
// reading never changes underneath it.
struct IssueModelRequest {
  TurnId turn_id{};
  std::shared_ptr<const ModelRequest> request{};
};

struct PublishTextDelta {
  TurnId turn_id{};
  std::string text{};
  std::uint32_t attempt{};
};

struct ScheduleRetryWake {
  MachineTimePoint deadline{};
};

struct PublishToolCall {
  TurnId turn_id{};
  ToolCallBlock call{};
  std::size_t remaining_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  std::uint32_t round{};
  std::uint32_t index{};
};

// The driver forwards this terminal intent to the pump as one value. The pump
// owns the atomic Conversation commit and callback delivery. The transcript is
// the request's own message list, so it opens with the turn's user message.
struct CommitCompletion {
  TurnId turn_id{};
  std::vector<Message> transcript{};
  FinishReason finish_reason{FinishReason::unknown};
  Usage usage{};
  std::uint32_t attempt_count{};
  std::string provider_request_id{};
  std::uint32_t tool_round_count{};
  std::uint32_t tool_call_count{};
  // Calls the final response asked for and the loop never dispatched, carried to
  // the host so it can see what the round limit cost it. They are deliberately
  // absent from the transcript: nothing ran, so nothing may be committed.
  std::vector<ToolCallBlock> unexecuted_tool_calls{};
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

// Every status but applied names why the event was rejected; a rejected event
// leaves the machine's state untouched and issues no commands.
enum class TransitionStatus : std::uint8_t {
  applied,
  ignored_terminal,
  event_not_allowed,
  wake_before_deadline,
  unknown_tool_call,
  duplicate_tool_result,
};

struct TransitionResult {
  std::vector<MachineCommand> commands{};
  TransitionStatus status{TransitionStatus::applied};
};

// The worker fills every field from Config; nothing here duplicates its defaults.
struct ToolLoopPolicy {
  std::uint32_t max_rounds{};
  std::size_t max_argument_bytes{};
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  ToolRoundLimitPolicy limit_policy{ToolRoundLimitPolicy::fail};
};

class TurnMachine {
public:
  TurnMachine(TurnId turn_id, ModelRequest request, RetryPolicy retry_policy,
              ToolLoopPolicy tool_policy);

  [[nodiscard]] TransitionResult apply(MachineEvent event);

  [[nodiscard]] MachinePhase phase() const noexcept;
  [[nodiscard]] std::uint32_t attempt_count() const noexcept;

private:
  struct QueuedState {};

  struct AwaitingModelState {};

  struct StreamingState {};

  struct RetryWaitState {
    MachineTimePoint deadline{};
    Error last_error{};
  };

  // The dispatched call's identity and the result that answers it. The block
  // itself is not held here: the committed assistant message already owns every
  // block of the round, and matching a result needs nothing but the ID.
  struct PendingToolCall {
    std::string id{};
    std::optional<ToolResultBlock> result{};
  };

  struct AwaitingToolState {
    Message assistant{};
    std::vector<PendingToolCall> calls{};
    std::size_t results_received{};
    std::string provider_request_id{};
  };

  struct TerminalState {};

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
  // Commits the response as the round's assistant message and publishes one
  // dispatch per tool call it carries.
  [[nodiscard]] TransitionResult begin_tool_round(ModelResponse response);
  [[nodiscard]] TransitionResult
  complete_turn(ModelResponse response, std::vector<ToolCallBlock> unexecuted = {});
  // Ends the turn at the round limit instead of failing it: the response's text
  // is committed and its calls are handed back undispatched.
  [[nodiscard]] TransitionResult complete_at_round_limit(ModelResponse response);
  [[nodiscard]] TransitionResult finish_error(Error error);
  [[nodiscard]] TransitionResult fail_response(ErrorCategory category,
                                               std::string message,
                                               std::string provider_request_id);
  [[nodiscard]] bool attempt_in_flight() const noexcept;
  // Validates the model response and rewrites each tool call in place with its
  // canonical arguments, so the committed assistant message and the dispatched
  // call carry the same bytes. Returns how many tool calls the response carries;
  // the blocks stay in the response rather than being copied out.
  [[nodiscard]] Result<std::size_t> validate_response(ModelResponse& response) const;
  [[nodiscard]] ModelRequest& mutable_request();
  [[nodiscard]] bool add_usage(const Usage& usage) noexcept;
  [[nodiscard]] bool reserve_exchange_bytes(std::size_t bytes) noexcept;
  [[nodiscard]] Error correlate(Error error) const;

  TurnId turn_id_{};
  // The turn's one transcript: the user message the turn opened with, every
  // committed tool round, and finally the assistant reply that ends it. The
  // machine resends it and hands it to the pump; nothing else holds a second
  // copy.
  std::shared_ptr<ModelRequest> request_{};
  RetryPolicy retry_policy_{};
  ToolLoopPolicy tool_policy_{};
  State state_{QueuedState{}};
  // When the current model request's retry window closes; set by start_request.
  MachineTimePoint retry_window_end_{};
  std::uint32_t attempt_count_{};
  std::uint32_t request_attempt_count_{};
  std::uint32_t tool_round_count_{};
  std::size_t exchange_payload_bytes_{};
  Usage usage_{};
  std::vector<std::string> dispatched_tool_ids_{};
};

} // namespace scry::detail
