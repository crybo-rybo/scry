#include "machine/turn_machine.hpp"

#include "core/json_codec.hpp"
#include "core/retry.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <string_view>
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

[[nodiscard]] Error response_error(const ErrorCategory category, std::string message) {
  return {
      .category = category,
      .message = std::move(message),
  };
}

[[nodiscard]] bool contains_id(const std::vector<std::string>& ids,
                               const std::string_view id) {
  return std::ranges::find(ids, id) != ids.end();
}

[[nodiscard]] Result<ToolCallBlock>
validated_call(const ToolCallBlock& call, const std::vector<std::string>& response_ids,
               const std::vector<std::string>& dispatched_ids,
               const std::size_t max_argument_bytes) {
  if (call.id.empty() || call.name.empty()) {
    return std::unexpected(response_error(
        ErrorCategory::protocol, "tool calls require non-empty IDs and names"));
  }
  if (call.arguments.text.size() > max_argument_bytes) {
    return std::unexpected(
        response_error(ErrorCategory::resource_limit,
                       "tool-call arguments exceed the configured byte limit"));
  }
  if (contains_id(response_ids, call.id) || contains_id(dispatched_ids, call.id)) {
    return std::unexpected(response_error(
        ErrorCategory::protocol, "tool-call IDs must be unique within a turn"));
  }
  auto arguments =
      canonicalize_json_object(call.arguments, ErrorCategory::protocol,
                               "tool-call arguments must be a JSON object");
  if (!arguments) {
    return std::unexpected(std::move(arguments.error()));
  }
  auto normalized = call;
  normalized.arguments = std::move(*arguments);
  return normalized;
}

} // namespace

TurnMachine::TurnMachine(TurnId turn_id, ModelRequest request, RetryPolicy retry_policy,
                         ToolLoopPolicy tool_policy)
    : turn_id_(turn_id), request_(std::make_shared<ModelRequest>(std::move(request))),
      retry_policy_(retry_policy), tool_policy_(tool_policy) {}

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
      MachinePhase::retry_wait, MachinePhase::awaiting_tool,  MachinePhase::terminal,
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
  return start_request(event.observed_at);
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

  auto calls = validate_response(event.response);
  if (!calls) {
    auto error = std::move(calls.error());
    error.provider_request_id = std::move(event.response.provider_request_id);
    return finish_error(correlate(std::move(error)));
  }
  if (!calls->empty() && tool_round_count_ >= tool_policy_.max_rounds) {
    return fail_response(ErrorCategory::max_tool_rounds,
                         "model exceeded the configured tool-round limit",
                         std::move(event.response.provider_request_id));
  }
  if (usage_would_overflow(event.response.usage)) {
    return fail_response(ErrorCategory::protocol,
                         "model usage counters overflow the turn total",
                         std::move(event.response.provider_request_id));
  }
  accumulate_usage(event.response.usage);

  if (!calls->empty()) {
    return begin_tool_round(std::move(event.response), std::move(*calls));
  }
  return complete_turn(std::move(event.response));
}

TransitionResult TurnMachine::on_event(AttemptFailed event) {
  const auto current_phase = phase();
  if (current_phase != MachinePhase::awaiting_model &&
      current_phase != MachinePhase::streaming) {
    return illegal(MachineEventKind::attempt_failed,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  if (event.observed_at < latest_time_.value_or(event.observed_at)) {
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

  const auto delay = retry_delay(retry_policy_, request_attempt_count_,
                                 event.retry_after, event.jitter_sample);
  const auto deadline = event.observed_at + delay;
  const auto elapsed_deadline =
      request_started_at_.value_or(event.observed_at) + bounded_elapsed(retry_policy_);
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
  if (event.observed_at < waiting->deadline) {
    return illegal(MachineEventKind::retry_wake,
                   TransitionDiagnosticReason::wake_before_deadline);
  }
  const auto elapsed_deadline =
      request_started_at_.value_or(event.observed_at) + bounded_elapsed(retry_policy_);
  if (event.observed_at > elapsed_deadline) {
    return finish_error(waiting->last_error);
  }
  latest_time_ = event.observed_at;
  return issue_attempt();
}

TransitionResult TurnMachine::on_event(ToolResultReady event) {
  auto* awaiting = std::get_if<AwaitingToolState>(&state_);
  if (awaiting == nullptr) {
    return illegal(MachineEventKind::tool_result_ready,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  const auto found =
      std::ranges::find(awaiting->calls, event.result.tool_call_id,
                        [](const PendingToolCall& pending) { return pending.call.id; });
  if (found == awaiting->calls.end()) {
    return illegal(MachineEventKind::tool_result_ready,
                   TransitionDiagnosticReason::unknown_tool_call);
  }
  if (found->result) {
    return illegal(MachineEventKind::tool_result_ready,
                   TransitionDiagnosticReason::duplicate_tool_result);
  }
  if (latest_time_ && event.observed_at < latest_time_.value()) {
    return illegal(MachineEventKind::tool_result_ready,
                   TransitionDiagnosticReason::non_monotonic_time);
  }
  const auto result_bytes = content_payload_bytes(event.result);
  if (!reserve_exchange_bytes(result_bytes)) {
    return fail_response(ErrorCategory::resource_limit,
                         "tool results exceed the remaining Conversation byte limit",
                         awaiting->provider_request_id);
  }

  found->result = std::move(event.result);
  ++awaiting->results_received;
  latest_time_ = event.observed_at;
  if (awaiting->results_received != awaiting->calls.size()) {
    return {};
  }

  Message results{.role = Role::user};
  results.content.reserve(awaiting->calls.size());
  for (auto& call : awaiting->calls) {
    // results_received reaches calls.size() only after every optional is populated.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    results.content.emplace_back(std::move(call.result).value());
  }
  auto assistant = std::move(awaiting->assistant);
  auto& request = mutable_request();
  request.messages.push_back(assistant);
  request.messages.push_back(results);
  exchange_.push_back(std::move(assistant));
  exchange_.push_back(std::move(results));
  return start_request(event.observed_at);
}

TransitionResult TurnMachine::on_event(ToolExecutionFailed event) {
  const auto* awaiting = std::get_if<AwaitingToolState>(&state_);
  if (awaiting == nullptr) {
    return illegal(MachineEventKind::tool_execution_failed,
                   TransitionDiagnosticReason::event_not_allowed);
  }
  if (event.error.provider_request_id.empty()) {
    event.error.provider_request_id = awaiting->provider_request_id;
  }
  return finish_error(correlate(std::move(event.error)));
}

TransitionResult TurnMachine::on_event(const CancelTurn /*event*/) {
  state_.emplace<TerminalState>(MachineTerminalKind::cancelled);
  return applied(PublishCancelled{.turn_id = turn_id_});
}

TransitionResult TurnMachine::start_request(const MachineTimePoint observed_at) {
  request_started_at_ = observed_at;
  latest_time_ = observed_at;
  request_attempt_count_ = 0;
  return issue_attempt();
}

TransitionResult TurnMachine::issue_attempt() {
  ++attempt_count_;
  ++request_attempt_count_;
  state_.emplace<AwaitingModelState>(attempt_count_);
  return applied(IssueModelRequest{
      .turn_id = turn_id_,
      .request = request_,
      .attempt = attempt_count_,
  });
}

ModelRequest& TurnMachine::mutable_request() {
  // An attempt in flight still holds the snapshot it was issued. Reseating
  // onto a private copy keeps that snapshot immutable for its reader; the
  // common case, where every issued attempt has already been consumed,
  // mutates in place and copies nothing.
  if (request_.use_count() > 1) {
    request_ = std::make_shared<ModelRequest>(*request_);
  }
  return *request_;
}

TransitionResult TurnMachine::begin_tool_round(ModelResponse response,
                                               std::vector<ToolCallBlock> calls) {
  Message assistant{
      .role = Role::assistant,
      .content = std::move(response.content),
  };
  if (!reserve_exchange_bytes(message_payload_bytes(assistant))) {
    return fail_response(ErrorCategory::resource_limit,
                         "tool response exceeds the remaining Conversation byte limit",
                         std::move(response.provider_request_id));
  }
  ++tool_round_count_;
  for (const auto& call : calls) {
    dispatched_tool_ids_.push_back(call.id);
  }

  AwaitingToolState awaiting{
      .assistant = std::move(assistant),
      .provider_request_id = std::move(response.provider_request_id),
  };
  awaiting.calls.reserve(calls.size());
  for (auto& call : calls) {
    awaiting.calls.push_back(PendingToolCall{.call = std::move(call)});
  }
  state_.emplace<AwaitingToolState>(std::move(awaiting));

  TransitionResult result{};
  const auto& pending = std::get<AwaitingToolState>(state_);
  result.commands.reserve(pending.calls.size());
  for (const auto& call : pending.calls) {
    result.commands.emplace_back(PublishToolCall{
        .turn_id = turn_id_,
        .call = call.call,
        .remaining_exchange_bytes = remaining_exchange_bytes(),
    });
  }
  return result;
}

TransitionResult TurnMachine::complete_turn(ModelResponse response) {
  Message assistant{
      .role = Role::assistant,
      .content = std::move(response.content),
  };
  if (!reserve_exchange_bytes(message_payload_bytes(assistant))) {
    return fail_response(ErrorCategory::resource_limit,
                         "completion exceeds the remaining Conversation byte limit",
                         std::move(response.provider_request_id));
  }
  exchange_.push_back(std::move(assistant));
  state_.emplace<TerminalState>(MachineTerminalKind::completed);
  return applied(CommitCompletion{
      .turn_id = turn_id_,
      .exchange = std::move(exchange_),
      .finish_reason = response.finish_reason,
      .usage = usage_,
      .attempt_count = attempt_count_,
      .provider_request_id = std::move(response.provider_request_id),
  });
}

TransitionResult TurnMachine::finish_error(Error error) {
  state_.emplace<TerminalState>(MachineTerminalKind::failed);
  return applied(PublishError{.error = std::move(error)});
}

TransitionResult TurnMachine::fail(const ErrorCategory category, std::string message) {
  return finish_error(correlate(response_error(category, std::move(message))));
}

TransitionResult TurnMachine::fail_response(const ErrorCategory category,
                                            std::string message,
                                            std::string provider_request_id) {
  auto error = response_error(category, std::move(message));
  error.provider_request_id = std::move(provider_request_id);
  return finish_error(correlate(std::move(error)));
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
  if (!is_retryable(error.category) ||
      request_attempt_count_ >= retry_policy_.max_attempts) {
    return false;
  }
  const auto deadline =
      request_started_at_.value_or(observed_at) + bounded_elapsed(retry_policy_);
  return observed_at <= deadline;
}

Result<std::vector<ToolCallBlock>>
TurnMachine::validate_response(const ModelResponse& response) const {
  std::vector<ToolCallBlock> calls;
  std::vector<std::string> response_ids;
  for (const auto& block : response.content) {
    if (std::holds_alternative<TextBlock>(block)) {
      continue;
    }
    const auto* call = std::get_if<ToolCallBlock>(&block);
    if (call == nullptr) {
      return std::unexpected(response_error(
          ErrorCategory::protocol, "assistant response contains a tool-result block"));
    }
    auto validated = validated_call(*call, response_ids, dispatched_tool_ids_,
                                    tool_policy_.max_argument_bytes);
    if (!validated) {
      return std::unexpected(std::move(validated.error()));
    }
    response_ids.push_back(call->id);
    calls.push_back(std::move(*validated));
  }

  const auto declares_tools = response.finish_reason == FinishReason::tool_use;
  if (declares_tools != !calls.empty()) {
    return std::unexpected(response_error(
        ErrorCategory::protocol,
        "tool-use finish reason and tool-call content are inconsistent"));
  }
  return calls;
}

bool TurnMachine::usage_would_overflow(const Usage& usage) const noexcept {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  return usage.input_tokens > maximum - usage_.input_tokens ||
         usage.output_tokens > maximum - usage_.output_tokens;
}

bool TurnMachine::reserve_exchange_bytes(const std::size_t bytes) noexcept {
  if (exchange_payload_bytes_ > tool_policy_.max_exchange_bytes ||
      bytes > tool_policy_.max_exchange_bytes - exchange_payload_bytes_) {
    return false;
  }
  exchange_payload_bytes_ += bytes;
  return true;
}

std::size_t TurnMachine::remaining_exchange_bytes() const noexcept {
  return tool_policy_.max_exchange_bytes - exchange_payload_bytes_;
}

void TurnMachine::accumulate_usage(const Usage& usage) noexcept {
  usage_.input_tokens += usage.input_tokens;
  usage_.output_tokens += usage.output_tokens;
}

Error TurnMachine::correlate(Error error) const {
  error.retryable = is_retryable(error.category);
  error.turn_id = turn_id_;
  error.attempt = attempt_count_;
  return error;
}

} // namespace scry::detail
