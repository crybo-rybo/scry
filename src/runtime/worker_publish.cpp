#include "runtime/worker.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

constexpr std::size_t terminal_event_reserve = 512;

[[nodiscard]] Error publication_error(const ErrorCategory category, std::string message,
                                      const TurnId turn_id,
                                      const std::uint32_t attempt = 0) {
  return Error{
      .category = category,
      .message = std::move(message),
      .turn_id = turn_id,
      .attempt = attempt,
  };
}

[[nodiscard]] WorkerEvent bound_terminal_event(WorkerEvent event) {
  auto* error = std::get_if<ErrorEvent>(&event);
  if (error == nullptr || event_payload_bytes(event) <= terminal_event_reserve) {
    return event;
  }
  error->error.message = "turn failed; diagnostic exceeded the event buffer";
  error->error.provider_detail.clear();
  error->error.provider_request_id.clear();
  return event;
}

} // namespace

Status
WorkerActor::publish_stream_events(TurnMachine& machine,
                                   const std::vector<ProviderEvent>& provider_events,
                                   std::optional<ModelResponse>& completed_response,
                                   const bool semantic_output_consumed) {
  if (semantic_output_consumed && machine.phase() == MachinePhase::awaiting_model) {
    const auto transition = machine.apply(ModelSemanticOutput{});
    if (transition.status != TransitionStatus::applied) {
      return std::unexpected(publication_error(
          ErrorCategory::invalid_state,
          "provider semantic output could not enter streaming state", TurnId{}));
    }
  }
  for (const auto& event : provider_events) {
    auto status = publish_provider_event(machine, event, completed_response);
    if (!status) {
      return status;
    }
  }
  return {};
}

Status
WorkerActor::publish_provider_event(TurnMachine& machine, const ProviderEvent& event,
                                    std::optional<ModelResponse>& completed_response) {
  if (const auto* text = std::get_if<ProviderTextDelta>(&event)) {
    auto transition = machine.apply(ModelTextDelta{.text = text->text});
    for (const auto& command : transition.commands) {
      auto status = publish_command(command);
      if (!status) {
        return status;
      }
    }
    return {};
  }
  if (const auto* completed = std::get_if<ProviderCompleted>(&event)) {
    if (completed_response) {
      return std::unexpected(publication_error(
          ErrorCategory::protocol, "provider stream emitted more than one completion",
          TurnId{}));
    }
    completed_response = completed->response;
    return {};
  }
  // The provider seam preserves an ignored event's name for debug inspection.
  // Scry has no public logging surface yet, so the worker consumes this marker.
  assert(std::holds_alternative<ProviderIgnoredEvent>(event));
  return {};
}

Status WorkerActor::publish_tool_batch(PublishToolCall first,
                                       std::deque<MachineCommand>& pending_commands) {
  const auto turn_id = first.turn_id;
  std::vector<WorkerEvent> events;
  events.emplace_back(ToolCallEvent{
      .turn_id = turn_id,
      .call = std::move(first.call),
      .remaining_exchange_bytes = first.remaining_exchange_bytes,
  });
  while (!pending_commands.empty()) {
    auto* next = std::get_if<PublishToolCall>(&pending_commands.front());
    if (next == nullptr || next->turn_id != turn_id) {
      break;
    }
    events.emplace_back(ToolCallEvent{
        .turn_id = turn_id,
        .call = std::move(next->call),
        .remaining_exchange_bytes = next->remaining_exchange_bytes,
    });
    pending_commands.pop_front();
  }
  const auto payload_limit =
      config_.limits.max_queued_event_bytes_per_turn - terminal_event_reserve;
  if (!events_->push_batch(std::move(events), payload_limit)) {
    return std::unexpected(publication_error(
        ErrorCategory::resource_limit,
        "tool-call batch exceeds the configured queue limit", turn_id));
  }
  return {};
}

void WorkerActor::publish_worker_tool_accepted(const TurnId turn_id,
                                               std::string tool_call_id,
                                               const std::size_t result_payload_bytes) {
  const auto payload_limit =
      config_.limits.max_queued_event_bytes_per_turn - terminal_event_reserve;
  const auto published = events_->push(
      WorkerToolAcceptedEvent{
          .turn_id = turn_id,
          .tool_call_id = std::move(tool_call_id),
          .result_payload_bytes = result_payload_bytes,
      },
      payload_limit);
  // Pump delivery releases the larger ToolCallEvent before posting execution.
  // With one worker producer, this smaller acknowledgement must therefore fit.
  static_cast<void>(published);
  assert(published);
}

Status WorkerActor::publish_command(const MachineCommand& command) {
  const auto payload_limit =
      config_.limits.max_queued_event_bytes_per_turn - terminal_event_reserve;
  if (const auto* delta = std::get_if<PublishTextDelta>(&command)) {
    if (!events_->push(TextDeltaEvent{.turn_id = delta->turn_id, .text = delta->text},
                       payload_limit)) {
      return std::unexpected(
          publication_error(ErrorCategory::resource_limit,
                            "turn events exceed the configured queue limit",
                            delta->turn_id, delta->attempt));
    }
    return {};
  }
  if (const auto* tool = std::get_if<PublishToolCall>(&command)) {
    if (!events_->push(
            ToolCallEvent{
                .turn_id = tool->turn_id,
                .call = tool->call,
                .remaining_exchange_bytes = tool->remaining_exchange_bytes,
            },
            payload_limit)) {
      return std::unexpected(publication_error(
          ErrorCategory::resource_limit,
          "turn events exceed the configured queue limit", tool->turn_id));
    }
    return {};
  }
  if (const auto* completion = std::get_if<CommitCompletion>(&command)) {
    if (!events_->push(
            CompletionEvent{
                .turn_id = completion->turn_id,
                .exchange = completion->exchange,
                .finish_reason = completion->finish_reason,
                .usage = completion->usage,
                .attempt_count = completion->attempt_count,
                .provider_request_id = completion->provider_request_id,
            },
            payload_limit)) {
      return std::unexpected(
          publication_error(ErrorCategory::resource_limit,
                            "turn events exceed the configured queue limit",
                            completion->turn_id, completion->attempt_count));
    }
    return {};
  }
  if (const auto* error = std::get_if<PublishError>(&command)) {
    const auto turn_id = error->error.turn_id.value_or(TurnId{});
    publish_terminal_event(ErrorEvent{.turn_id = turn_id, .error = error->error});
    return {};
  }
  if (const auto* cancelled = std::get_if<PublishCancelled>(&command)) {
    publish_terminal_event(CancelledEvent{.turn_id = cancelled->turn_id});
  }
  return {};
}

void WorkerActor::publish_terminal_event(WorkerEvent event) {
  event = bound_terminal_event(std::move(event));
  const auto pushed = events_->push_terminal(
      std::move(event), config_.limits.max_queued_event_bytes_per_turn);
  static_cast<void>(pushed);
  assert(pushed);
}

void WorkerActor::publish_unhandled_failure(const TurnId turn_id) noexcept {
  try {
    publish_terminal_event(ErrorEvent{
        .turn_id = turn_id,
        .error =
            publication_error(ErrorCategory::invalid_state,
                              "worker could not process the accepted turn", turn_id),
    });
  } catch (...) {
    // Allocation failure is outside Scry's semantic-failure contract. The
    // thread boundary still never permits an exception to escape.
    return;
  }
}

} // namespace scry::detail
