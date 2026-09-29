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

// The host accepted the answer a typed turn's response tool was called with. A
// rejected answer arrives as an ordinary ToolResultReady carrying the error the
// model is given.
struct AnswerAccepted {
  std::string call_id{};
};

struct CancelTurn {};

using MachineEvent =
    std::variant<BeginTurn, ModelTextDelta, ModelSemanticOutput, ModelCompleted,
                 AttemptFailed, RetryWake, ToolResultReady, ToolExecutionFailed,
                 AnswerAccepted, CancelTurn>;

// Tells the worker to send one model request. The request is shared rather than
// copied, because retries and tool rounds resend the same conversation; the
// machine copies it before any change while an attempt still holds it, so the
// snapshot an attempt is reading never changes underneath it.
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

// What the host thread does with a published call. Only `tool` reaches a handler,
// the per-turn call limit, and the host's tool hooks; the other two are calls to
// a typed turn's response tool, which Scry answers itself.
enum class ToolCallRole : std::uint8_t {
  // A registered (or unknown) tool: dispatched as usual.
  tool,
  // The response's only call, to the response tool: its arguments are the
  // candidate answer, validated on the host thread.
  answer,
  // A response-tool call made beside other calls, or more than once: refused
  // with a tool error telling the model to call it on its own.
  misplaced_answer,
};

struct PublishToolCall {
  TurnId turn_id{};
  ToolCallBlock call{};
  std::size_t remaining_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  std::uint32_t round{};
  std::uint32_t index{};
  ToolCallRole role{ToolCallRole::tool};
};

// The worker forwards this terminal intent to the pump as one value. The pump
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
  // True when the turn ended on an accepted answer. The transcript's final
  // assistant message then ends with a text block holding the answer's canonical
  // JSON, which stands in for the response-tool call.
  bool answered{false};
  std::uint32_t answer_attempt_count{};
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

// The worker sets every field for each turn: max_exchange_bytes from what the
// Conversation byte limit leaves after the user message, the rest from Config.
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
    // The round's only call is a candidate answer. Its acceptance ends the turn;
    // its rejection makes the round an ordinary one, counted only then.
    bool answer_candidate{false};
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
  [[nodiscard]] TransitionResult on_event(AnswerAccepted event);
  [[nodiscard]] TransitionResult on_event(CancelTurn event);

  [[nodiscard]] TransitionResult start_request(MachineTimePoint observed_at);
  [[nodiscard]] TransitionResult issue_attempt();
  // A typed turn's response: it must call a tool, and only a lone response-tool
  // call can end the turn, so every other shape at the round limit fails it.
  [[nodiscard]] TransitionResult typed_response(ModelResponse response,
                                                std::size_t call_count);
  // Commits the response as the round's assistant message and publishes one
  // dispatch per tool call it carries. A candidate answer's round is counted only
  // once the answer is rejected.
  [[nodiscard]] TransitionResult begin_tool_round(ModelResponse response,
                                                  bool answer_candidate = false);
  [[nodiscard]] ToolCallRole role_of(const ToolCallBlock& call,
                                     bool answer_candidate) const noexcept;
  // Closes a round once every result is in and resends the transcript.
  [[nodiscard]] TransitionResult finish_tool_round(AwaitingToolState& awaiting,
                                                   MachineTimePoint observed_at);
  [[nodiscard]] TransitionResult
  complete_turn(ModelResponse response, std::vector<ToolCallBlock> unexecuted = {});
  // Ends the turn on an accepted answer: the response-tool call becomes a text
  // block holding its arguments, so no call is committed without a result.
  [[nodiscard]] TransitionResult complete_with_answer(AwaitingToolState& awaiting);
  // Appends the final assistant message, already reserved, and publishes the
  // commit.
  [[nodiscard]] TransitionResult
  commit_transcript(Message assistant, FinishReason finish_reason,
                    std::string provider_request_id,
                    std::vector<ToolCallBlock> unexecuted, bool answered);
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
  // Host tool calls, and response-tool calls, published across the turn.
  std::uint32_t tool_call_count_{};
  std::uint32_t answer_attempt_count_{};
  std::size_t exchange_payload_bytes_{};
  Usage usage_{};
  // Every call ID published this turn, response-tool calls included, so no ID is
  // answered twice.
  std::vector<std::string> dispatched_tool_ids_{};
  // Name of the response tool of a typed turn; empty for an untyped one.
  std::string answer_tool_{};
};

} // namespace scry::detail
