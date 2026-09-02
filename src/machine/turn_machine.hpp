/// @file
/// @brief Deterministic sans-I/O state machine for one agentic turn.
///
/// `TurnMachine` consumes typed events and emits typed commands; it performs no
/// network, clock, queue, callback, or tool I/O. The worker supplies observed
/// time and executes commands, making every transition replayable in tests.
/// Illegal events leave state untouched and return a structured diagnostic.

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

/// @brief Clock domain used for injected retry observations and deadlines.
using MachineTimePoint = std::chrono::steady_clock::time_point;

/// @brief Coarse observable phase of the internal turn state machine.
enum class MachinePhase : std::uint8_t {
  queued,         ///< Accepted but not yet issued to a provider.
  awaiting_model, ///< A model attempt is active; no semantic output seen yet.
  streaming,      ///< Semantic output has begun, so automatic retry is disabled.
  retry_wait,     ///< A retryable attempt failed and awaits an injected wake.
  awaiting_tool,  ///< An ordered tool-call batch awaits app-thread results.
  terminal,       ///< Completion, failure, or cancellation has been published.
};

/// @brief Distinguishes the three possible terminal outcomes.
enum class MachineTerminalKind : std::uint8_t {
  completed, ///< A complete exchange is ready for transactional commit.
  failed,    ///< The turn ended with a categorized error; commit nothing.
  cancelled, ///< Cooperative cancellation ended the turn; commit nothing.
};

/// @brief Starts a newly queued turn at an injected clock observation.
struct BeginTurn {
  MachineTimePoint observed_at{}; ///< Time used to start this request's retry window.
};

/// @brief Semantic text decoded from the active model attempt.
struct ModelTextDelta {
  std::string text{}; ///< Owning text bytes to publish to the pump.
};

/// @brief Marks non-text semantic output that also disables automatic retry.
///
/// Providers emit this for meaningful stream content that does not itself
/// produce an app-facing text command.
struct ModelSemanticOutput {};

/// @brief Supplies a completely decoded response for the active attempt.
struct ModelCompleted {
  ModelResponse response{}; ///< Owning neutral response to validate and consume.
};

/// @brief Reports a failed model/transport attempt with retry inputs.
struct AttemptFailed {
  Error error{}; ///< Categorized failure; correlation is added by the machine.
  MachineTimePoint observed_at{}; ///< Injected failure observation time.
  std::optional<std::chrono::milliseconds> retry_after{}; ///< Provider minimum delay.
  double jitter_sample{}; ///< Deterministic jitter sample, clamped to `[-1, 1]`.
};

/// @brief Announces that the driver has reached a scheduled retry deadline.
struct RetryWake {
  MachineTimePoint observed_at{}; ///< Injected wake time; early wakes are illegal.
};

/// @brief Returns one app-thread tool result to the machine.
struct ToolResultReady {
  ToolResultBlock result{};       ///< Result keyed by the original tool-call ID.
  MachineTimePoint observed_at{}; ///< Injected completion time for ordering checks.
};

/// @brief Reports a fatal framework failure during pump-side tool dispatch.
struct ToolExecutionFailed {
  Error error{}; ///< Error to publish after adding missing turn correlation.
};

/// @brief Requests cooperative cancellation from any nonterminal phase.
struct CancelTurn {};

/// @brief Closed event vocabulary accepted by `TurnMachine::apply()`.
using MachineEvent = std::variant<BeginTurn, ModelTextDelta, ModelSemanticOutput,
                                  ModelCompleted, AttemptFailed, RetryWake,
                                  ToolResultReady, ToolExecutionFailed, CancelTurn>;

/// @brief Stable event labels used in transition diagnostics.
enum class MachineEventKind : std::uint8_t {
  begin,                 ///< `BeginTurn`.
  text_delta,            ///< `ModelTextDelta`.
  semantic_output,       ///< `ModelSemanticOutput`.
  completed,             ///< `ModelCompleted`.
  attempt_failed,        ///< `AttemptFailed`.
  retry_wake,            ///< `RetryWake`.
  tool_result_ready,     ///< `ToolResultReady`.
  tool_execution_failed, ///< `ToolExecutionFailed`.
  cancel,                ///< `CancelTurn`.
};

/// @brief Commands the worker to issue one immutable model-request snapshot.
///
/// Retries can share the same request allocation. Before a later tool round
/// appends messages, the machine reseats its copy when an issued attempt still
/// owns the snapshot, preserving the immutable view retained by that attempt.
struct IssueModelRequest {
  TurnId turn_id{};                              ///< Immutable routing identity.
  std::shared_ptr<const ModelRequest> request{}; ///< Attempt-owned request snapshot.
  std::uint32_t attempt{}; ///< One-based aggregate attempt number for the turn.
};

/// @brief Commands the pump to publish one decoded text delta.
struct PublishTextDelta {
  TurnId turn_id{};        ///< Immutable routing identity.
  std::string text{};      ///< Owning text bytes transferred to the event queue.
  std::uint32_t attempt{}; ///< Attempt that produced the delta.
};

/// @brief Commands the driver to inject a future retry wake event.
struct ScheduleRetryWake {
  TurnId turn_id{};               ///< Immutable routing identity.
  MachineTimePoint deadline{};    ///< Earliest legal `RetryWake` observation.
  std::uint32_t failed_attempt{}; ///< Aggregate attempt that just failed.
};

/// @brief Commands the pump to execute one call from an atomic tool batch.
struct PublishToolCall {
  TurnId turn_id{};     ///< Immutable routing identity.
  ToolCallBlock call{}; ///< Validated call with canonical object arguments.
  std::size_t remaining_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  ///< Authoritative Conversation budget available before this batch's results.
};

/// @brief Commands the pump to commit and publish one successful turn.
///
/// The driver forwards this terminal intent as one value. Only the pump mutates
/// Conversation history: it prepends the route's original user message, commits the
/// resulting exchange, and then retains or delivers the optional terminal callback.
struct CommitCompletion {
  TurnId turn_id{};                ///< Immutable routing identity.
  std::vector<Message> exchange{}; ///< Assistant/tool-result/final suffix to commit.
  FinishReason finish_reason{FinishReason::unknown}; ///< Final neutral stop cause.
  Usage usage{}; ///< Overflow-checked aggregate across all model requests.
  std::uint32_t attempt_count{};     ///< Aggregate attempts across every tool round.
  std::string provider_request_id{}; ///< Final provider correlation identifier.
};

/// @brief Commands terminal failure publication without a Conversation commit.
struct PublishError {
  Error error{}; ///< Fully correlated terminal error.
};

/// @brief Commands terminal cancellation publication without a commit.
struct PublishCancelled {
  TurnId turn_id{}; ///< Immutable routing identity.
};

/// @brief Closed command vocabulary emitted by the machine.
using MachineCommand =
    std::variant<IssueModelRequest, PublishTextDelta, ScheduleRetryWake,
                 PublishToolCall, CommitCompletion, PublishError, PublishCancelled>;

/// @brief Outcome classification for one attempted transition.
enum class TransitionStatus : std::uint8_t {
  applied,            ///< Event was accepted; zero or more commands may result.
  ignored_terminal,   ///< Terminal state absorbs the event without side effects.
  illegal_transition, ///< Event was rejected and state remained unchanged.
};

/// @brief Machine-readable reason an event could not be applied.
enum class TransitionDiagnosticReason : std::uint8_t {
  event_not_allowed,     ///< Event is invalid in the current phase.
  non_monotonic_time,    ///< Injected observation predates an accepted one.
  wake_before_deadline,  ///< Retry wake arrived before its requested deadline.
  unknown_tool_call,     ///< Result does not match the pending batch.
  duplicate_tool_result, ///< The matching pending call already has a result.
};

/// @brief Structured context for an illegal transition.
struct TransitionDiagnostic {
  MachinePhase phase{MachinePhase::queued}; ///< Phase in which rejection occurred.
  MachineEventKind event{MachineEventKind::begin}; ///< Rejected event kind.
  TransitionDiagnosticReason reason{TransitionDiagnosticReason::event_not_allowed};
  ///< Precise validation failure.
};

/// @brief Commands and status produced by one event application.
struct TransitionResult {
  std::vector<MachineCommand> commands{}; ///< Ordered side effects for the driver.
  TransitionStatus status{TransitionStatus::applied}; ///< Application outcome.
  std::optional<TransitionDiagnostic> diagnostic{};   ///< Present only when illegal.
};

/// @brief Resource policy enforced across all tool rounds in one turn.
struct ToolLoopPolicy {
  std::uint32_t max_rounds{8}; ///< Maximum model responses that request tools.
  std::size_t max_argument_bytes{std::size_t{1024} * 1024};
  ///< Maximum canonical argument bytes for any one call.
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
  ///< Cumulative assistant/tool-result/final payload budget.
};

/// @brief Pure state machine that owns one accepted turn's agentic loop.
///
/// The machine validates provider responses, controls pre-semantic-output
/// retry, enforces at-most-once tool-call IDs and cumulative exchange limits,
/// and emits exactly one terminal intent. It is exclusively worker-owned.
///
/// @invariant Nonterminal events mutate state only when returned status is
///            `TransitionStatus::applied`.
/// @invariant Tool-call identifiers are dispatched at most once per turn.
class TurnMachine {
public:
  /// @brief Creates a queued machine with its immutable request inputs.
  /// @param turn_id Identity copied into every command and error.
  /// @param request Initial request containing committed history and user input.
  /// @param retry_policy Per-model-request retry limits and backoff parameters.
  /// @param tool_policy Turn-wide tool-round and payload limits.
  TurnMachine(TurnId turn_id, ModelRequest request, RetryPolicy retry_policy,
              ToolLoopPolicy tool_policy = {});

  /// @brief Applies one event and returns all resulting driver commands.
  /// @param event Owning event to consume.
  /// @return Transition status, optional diagnostic, and ordered commands.
  /// @note Events applied after terminal state are harmless and reported as
  ///       `ignored_terminal`.
  [[nodiscard]] TransitionResult apply(MachineEvent event);

  /// @brief Returns the current coarse phase.
  /// @return Phase corresponding to the active state alternative.
  [[nodiscard]] MachinePhase phase() const noexcept;
  /// @brief Returns attempts issued across all requests/tool rounds.
  /// @return Aggregate one-based-at-issue attempt count; zero before begin.
  [[nodiscard]] std::uint32_t attempt_count() const noexcept;
  /// @brief Returns the terminal outcome, if terminal.
  /// @return Terminal kind or `nullopt` while the turn remains active.
  [[nodiscard]] std::optional<MachineTerminalKind> terminal_kind() const noexcept;

private:
  /// @brief Marker state before the first model request is issued.
  struct QueuedState {};

  /// @brief Active attempt that has not consumed semantic output.
  struct AwaitingModelState {
    std::uint32_t attempt{}; ///< Aggregate attempt number used for text events.
  };

  /// @brief Active attempt after semantic output makes retry unsafe.
  struct StreamingState {
    std::uint32_t attempt{}; ///< Aggregate attempt number used for text events.
  };

  /// @brief Suspended retry containing the earliest legal wake and last error.
  struct RetryWaitState {
    MachineTimePoint deadline{}; ///< Earliest accepted wake observation.
    Error last_error{};          ///< Correlated error published if the window expires.
  };

  /// @brief One validated call and its not-yet-or-already-returned result.
  struct PendingToolCall {
    ToolCallBlock call{};                    ///< Canonical call published exactly once.
    std::optional<ToolResultBlock> result{}; ///< Populated once by matching ID.
  };

  /// @brief Atomic provider batch waiting for ordered app-thread tool results.
  struct AwaitingToolState {
    Message assistant{}; ///< Assistant call message retained for the exchange.
    std::vector<PendingToolCall> calls{}; ///< Provider-ordered pending calls.
    std::size_t results_received{};       ///< Number of populated result optionals.
    std::string provider_request_id{};    ///< Correlation ID for dispatch failures.
  };

  /// @brief Absorbing state reached immediately before terminal publication.
  struct TerminalState {
    MachineTerminalKind kind{MachineTerminalKind::failed}; ///< Final outcome.
  };

  /// @brief Closed state alternative; its index maps directly to `MachinePhase`.
  using State = std::variant<QueuedState, AwaitingModelState, StreamingState,
                             RetryWaitState, AwaitingToolState, TerminalState>;

  /// @brief Handles initial issue from queued state.
  /// @param event Initial timestamp observation.
  /// @return First request command, or an illegal-transition diagnostic.
  [[nodiscard]] TransitionResult on_event(BeginTurn event);
  /// @brief Publishes text and enters/remains in streaming state.
  /// @param event Owning text delta from the current attempt.
  /// @return Text publication command, or an illegal-transition diagnostic.
  [[nodiscard]] TransitionResult on_event(ModelTextDelta event);
  /// @brief Marks semantic output without publishing text.
  /// @param event Marker event.
  /// @return Applied empty result, or an illegal-transition diagnostic.
  [[nodiscard]] TransitionResult on_event(ModelSemanticOutput event);
  /// @brief Validates a complete response and branches to tools or completion.
  /// @param event Owning decoded response.
  /// @return Tool publications, terminal commit/error, or illegal diagnostic.
  [[nodiscard]] TransitionResult on_event(ModelCompleted event);
  /// @brief Applies cancellation/retry/terminal policy to an attempt failure.
  /// @param event Owning failure and injected retry inputs.
  /// @return Retry schedule, terminal publication, or illegal diagnostic.
  [[nodiscard]] TransitionResult on_event(AttemptFailed event);
  /// @brief Issues the next attempt when a scheduled wake is valid.
  /// @param event Injected wake observation.
  /// @return Request/error command, or an illegal-transition diagnostic.
  [[nodiscard]] TransitionResult on_event(RetryWake event);
  /// @brief Records one tool result and resends after the full batch arrives.
  /// @param event Owning result and injected observation.
  /// @return Empty applied result, next request/error, or illegal diagnostic.
  [[nodiscard]] TransitionResult on_event(ToolResultReady event);
  /// @brief Terminates an awaiting-tool state after a framework dispatch error.
  /// @param event Owning failure from pump-side dispatch.
  /// @return Terminal error command, or an illegal-transition diagnostic.
  [[nodiscard]] TransitionResult on_event(ToolExecutionFailed event);
  /// @brief Cancels any nonterminal phase and releases the request snapshot.
  /// @param event Marker event.
  /// @return Exactly one cancellation publication command.
  [[nodiscard]] TransitionResult on_event(CancelTurn event);

  /// @brief Resets per-request retry counters and issues its first attempt.
  /// @param observed_at Injected start time for elapsed retry bounds.
  /// @return Exactly one model-request command.
  [[nodiscard]] TransitionResult start_request(MachineTimePoint observed_at);
  /// @brief Increments counters and emits a snapshot-bearing request command.
  /// @return Exactly one model-request command.
  [[nodiscard]] TransitionResult issue_attempt();
  /// @brief Reserves and publishes a validated atomic tool-call batch.
  /// @param response Owning provider response whose content is retained.
  /// @param calls Validated canonical calls in provider order.
  /// @return One command per call, or a terminal resource error.
  [[nodiscard]] TransitionResult begin_tool_round(ModelResponse response,
                                                  std::vector<ToolCallBlock> calls);
  /// @brief Reserves final content and emits the transactional commit intent.
  /// @param response Owning final response.
  /// @return One terminal commit command, or a terminal resource error.
  [[nodiscard]] TransitionResult complete_turn(ModelResponse response);
  /// @brief Enters failed terminal state and emits one error command.
  /// @param error Fully categorized and normally correlated error.
  /// @return Exactly one terminal error command.
  [[nodiscard]] TransitionResult finish_error(Error error);
  /// @brief Builds, correlates, and publishes a response-validation failure.
  /// @param category Stable terminal category.
  /// @param message Sanitized diagnostic.
  /// @param provider_request_id Provider correlation value to preserve.
  /// @return Exactly one correlated terminal error command.
  [[nodiscard]] TransitionResult fail_response(ErrorCategory category,
                                               std::string message,
                                               std::string provider_request_id);
  /// @brief Creates a side-effect-free illegal-transition result.
  /// @param event Rejected event label.
  /// @param reason Exact rejection reason.
  /// @return Diagnostic result with no commands.
  [[nodiscard]] TransitionResult illegal(MachineEventKind event,
                                         TransitionDiagnosticReason reason) const;
  /// @brief Checks category, attempt cap, and elapsed cap for another attempt.
  /// @param error Correlated attempt failure.
  /// @param observed_at Injected failure time.
  /// @return Whether a new attempt may still be scheduled.
  [[nodiscard]] bool retry_is_allowed(const Error& error,
                                      MachineTimePoint observed_at) const noexcept;
  /// @brief Validates a response and canonicalizes every tool call in place.
  ///
  /// Rewriting the response ensures the committed assistant message and the
  /// dispatched call carry identical JSON bytes. IDs must be nonempty and
  /// unique across the entire turn, arguments must be bounded JSON objects,
  /// and the finish reason must agree with the presence of calls.
  ///
  /// @param response Mutable response whose calls are normalized.
  /// @return Provider-ordered call copies, or a protocol/resource error.
  [[nodiscard]] Result<std::vector<ToolCallBlock>>
  validate_response(ModelResponse& response) const;
  /// @brief Returns a private mutable request, reseating copy-on-write if shared.
  /// @return Mutable reference valid while this machine retains `request_`.
  [[nodiscard]] ModelRequest& mutable_request();
  /// @brief Checks aggregate token counters before addition.
  /// @param usage Counters from the newly completed model response.
  /// @return Whether either aggregate addition would overflow.
  [[nodiscard]] bool usage_would_overflow(const Usage& usage) const noexcept;
  /// @brief Atomically reserves bytes from the cumulative exchange budget.
  /// @param bytes Payload bytes to reserve.
  /// @return Whether the reservation fits; failure leaves accounting unchanged.
  [[nodiscard]] bool reserve_exchange_bytes(std::size_t bytes) noexcept;
  /// @brief Returns the unreserved portion of the exchange budget.
  /// @return Remaining bytes after all successful reservations.
  [[nodiscard]] std::size_t remaining_exchange_bytes() const noexcept;
  /// @brief Adds request usage after `usage_would_overflow()` succeeds.
  /// @param usage Counters to add to the turn aggregate.
  void accumulate_usage(const Usage& usage) noexcept;
  /// @brief Attaches retry classification, turn ID, and current attempt.
  /// @param error Error value to enrich.
  /// @return Correlated owning error.
  [[nodiscard]] Error correlate(Error error) const;

  TurnId turn_id_{}; ///< Immutable routing and correlation identity.
  std::shared_ptr<ModelRequest> request_{}; ///< COW request owned until terminal.
  RetryPolicy retry_policy_{};              ///< Per-model-request retry controls.
  ToolLoopPolicy tool_policy_{};            ///< Turn-wide tool and exchange limits.
  State state_{QueuedState{}};              ///< Current explicit machine state.
  std::vector<Message> exchange_{};         ///< Successful uncommitted turn suffix.
  std::optional<MachineTimePoint> request_started_at_{};
  ///< Beginning of the current model request's elapsed retry window.
  std::optional<MachineTimePoint> latest_time_{};
  ///< Latest accepted injected observation for monotonicity checks.
  std::uint32_t attempt_count_{};         ///< Aggregate attempts across the whole turn.
  std::uint32_t request_attempt_count_{}; ///< Attempts in the current tool round.
  std::uint32_t tool_round_count_{};      ///< Validated tool-use responses accepted.
  std::size_t exchange_payload_bytes_{};  ///< Bytes reserved from Conversation budget.
  Usage usage_{}; ///< Aggregate usage across all completed model requests.
  std::vector<std::string> dispatched_tool_ids_{};
  ///< Turn-wide at-most-once ledger of published tool call IDs.
};

} // namespace scry::detail
