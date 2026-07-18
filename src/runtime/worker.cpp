#include "runtime/worker.hpp"

#include "protocol/sse.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace scry::detail {
namespace {

constexpr std::size_t terminal_event_reserve = 512;

[[nodiscard]] Error worker_error(const ErrorCategory category, std::string message,
                                 const TurnId turn_id,
                                 const std::uint32_t attempt = 0) {
  return Error{
      .category = category,
      .message = std::move(message),
      .turn_id = turn_id,
      .attempt = attempt,
  };
}

void append_commands(std::deque<MachineCommand>& destination,
                     TransitionResult transition) {
  for (auto& command : transition.commands) {
    destination.push_back(std::move(command));
  }
}

[[nodiscard]] double jitter_sample(const TurnId turn_id,
                                   const std::uint32_t attempt) noexcept {
  auto value = turn_id.value ^ (static_cast<std::uint64_t>(attempt) *
                                std::uint64_t{0x9E3779B97F4A7C15});
  value ^= value >> 12U;
  value ^= value << 25U;
  value ^= value >> 27U;
  const auto unit = static_cast<double>(value & std::uint64_t{0xFFFF}) / 65535.0;
  return (unit * 2.0) - 1.0;
}

[[nodiscard]] bool contains_tool_content(const ModelResponse& response) noexcept {
  return response.finish_reason == FinishReason::tool_use ||
         std::ranges::any_of(response.content, [](const auto& block) {
           return !std::holds_alternative<TextBlock>(block);
         });
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

void redact_sensitive_fields(Error& error, const std::string_view secret) {
  if (secret.empty()) {
    return;
  }
  if (error.message.find(secret) != std::string::npos) {
    error.message = "operation failed; sensitive diagnostic redacted";
  }
  if (error.provider_detail.find(secret) != std::string::npos) {
    error.provider_detail.clear();
  }
  if (error.provider_request_id.find(secret) != std::string::npos) {
    error.provider_request_id.clear();
  }
}

[[nodiscard]] TransitionResult failed_attempt(TurnMachine& machine, Error error,
                                              const TurnId turn_id,
                                              const std::string_view secret) {
  redact_sensitive_fields(error, secret);
  const auto attempt = machine.attempt_count();
  const auto retry_after = error.retry_after;
  if (!error.turn_id) {
    error.turn_id = turn_id;
  }
  if (error.attempt == 0) {
    error.attempt = attempt;
  }
  return machine.apply(AttemptFailed{
      .error = std::move(error),
      .observed_at = std::chrono::steady_clock::now(),
      .retry_after = retry_after,
      .jitter_sample = jitter_sample(turn_id, attempt),
  });
}

} // namespace

WorkerActor::WorkerActor(Config config, std::unique_ptr<ProviderAdapter> provider,
                         std::unique_ptr<Transport> transport,
                         std::shared_ptr<CommandQueue> commands,
                         std::shared_ptr<EventQueue> events)
    : config_(std::move(config)), provider_(std::move(provider)),
      transport_(std::move(transport)), commands_(std::move(commands)),
      events_(std::move(events)) {}

struct WorkerActor::AttemptState {
  explicit AttemptState(const std::size_t maximum_event_bytes)
      : parser(maximum_event_bytes) {}

  SseParser parser;
  ProviderDecodeState decode{};
  std::optional<ModelResponse> completed{};
};

void WorkerActor::run(const std::stop_token& stopped) noexcept {
  while (!stopped.stop_requested()) {
    if (pending_.empty()) {
      auto command = commands_->wait_pop(stopped);
      if (!command) {
        return;
      }
      accept_command(std::move(*command));
    }
    while (auto command = commands_->try_pop()) {
      accept_command(std::move(*command));
    }
    if (pending_.empty()) {
      continue;
    }
    auto command = std::move(pending_.front());
    pending_.pop_front();
    const auto turn_id = command.turn_id;
    try {
      process_turn(std::move(command), stopped);
    } catch (...) {
      publish_unhandled_failure(turn_id);
    }
  }
}

void WorkerActor::accept_command(WorkerCommand command) {
  if (auto* send = std::get_if<SendTurnCommand>(&command)) {
    pending_.push_back(std::move(*send));
    return;
  }
  const auto turn_id = std::get<CancelTurnCommand>(command).turn_id;
  const auto found = std::ranges::find(pending_, turn_id, &SendTurnCommand::turn_id);
  if (found != pending_.end()) {
    pending_.erase(found);
    publish_terminal_event(CancelledEvent{.turn_id = turn_id});
  }
}

void WorkerActor::process_turn(SendTurnCommand&& command,
                               const std::stop_token& stopped) {
  TurnMachine machine{command.turn_id, std::move(command.request), config_.retry};
  std::deque<MachineCommand> machine_commands;
  if (command.cancelled->load(std::memory_order_acquire)) {
    append_commands(machine_commands, machine.apply(CancelTurn{}));
  } else {
    append_commands(machine_commands,
                    machine.apply(BeginTurn{
                        .observed_at = std::chrono::steady_clock::now(),
                    }));
  }

  while (!machine_commands.empty() && !stopped.stop_requested()) {
    auto next = std::move(machine_commands.front());
    machine_commands.pop_front();
    if (const auto* issue = std::get_if<IssueModelRequest>(&next)) {
      append_commands(machine_commands,
                      perform_attempt(machine, *issue, command.cancelled, stopped));
      continue;
    }
    if (const auto* wake = std::get_if<ScheduleRetryWake>(&next)) {
      append_commands(machine_commands,
                      wait_for_retry(machine, *wake, command.cancelled, stopped));
      continue;
    }
    auto published = publish_command(next);
    if (!published) {
      if (machine.phase() == MachinePhase::terminal) {
        auto error = worker_error(ErrorCategory::resource_limit,
                                  "turn events exceed the configured queue limit",
                                  command.turn_id, machine.attempt_count());
        publish_terminal_event(
            ErrorEvent{.turn_id = command.turn_id, .error = std::move(error)});
        return;
      }
      append_commands(machine_commands,
                      failed_attempt(machine, std::move(published.error()),
                                     command.turn_id, config_.api_key));
    }
  }
}

TransitionResult
WorkerActor::perform_attempt(TurnMachine& machine, const IssueModelRequest& issue,
                             const std::shared_ptr<std::atomic<bool>>& cancelled,
                             const std::stop_token& stopped) {
  auto request = provider_->make_request(config_, issue.request);
  if (!request) {
    return failed_attempt(machine, std::move(request.error()), issue.turn_id,
                          config_.api_key);
  }

  assert(request->streaming);
  AttemptState state{config_.limits.max_sse_event_bytes};
  BodyChunkSink body_sink{
      [this, &machine, &state](const std::string_view chunk) -> Status {
        return consume_stream_chunk(machine, state, chunk);
      }};

  auto result = transport_->perform(*request, stopped, *cancelled, body_sink);
  if (!result) {
    return failed_attempt(machine, std::move(result.error()), issue.turn_id,
                          config_.api_key);
  }
  auto response = finish_stream(machine, state);
  if (!response) {
    return failed_attempt(machine, std::move(response.error()), issue.turn_id,
                          config_.api_key);
  }
  return complete_attempt(machine, std::move(*response), *result, issue);
}

Status WorkerActor::consume_stream_chunk(TurnMachine& machine, AttemptState& state,
                                         const std::string_view chunk) {
  auto parsed = state.parser.push(chunk);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return consume_sse_events(machine, state, *parsed);
}

Status WorkerActor::consume_sse_events(TurnMachine& machine, AttemptState& state,
                                       const std::vector<SseEvent>& events) {
  for (const auto& event : events) {
    auto provider_events =
        provider_->parse_stream_event(event.name, event.data, state.decode);
    if (!provider_events) {
      return std::unexpected(std::move(provider_events.error()));
    }
    auto status = publish_stream_events(machine, *provider_events, state.completed,
                                        state.decode.semantic_output_consumed);
    if (!status) {
      return status;
    }
  }
  return {};
}

Result<ModelResponse> WorkerActor::finish_stream(TurnMachine& machine,
                                                 AttemptState& state) {
  auto trailing = state.parser.finish();
  if (!trailing) {
    return std::unexpected(std::move(trailing.error()));
  }
  if (auto status = consume_sse_events(machine, state, *trailing); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (!state.completed) {
    return std::unexpected(
        worker_error(ErrorCategory::protocol,
                     "provider stream ended without a completion event", TurnId{}));
  }
  return std::move(*state.completed);
}

TransitionResult WorkerActor::complete_attempt(TurnMachine& machine,
                                               ModelResponse response,
                                               const TransportResult& result,
                                               const IssueModelRequest& issue) {
  if (response.provider_request_id.empty()) {
    response.provider_request_id = result.provider_request_id;
  }
  if (response.provider_request_id.find(config_.api_key) != std::string::npos) {
    response.provider_request_id.clear();
  }
  if (contains_tool_content(response)) {
    return failed_attempt(
        machine,
        worker_error(ErrorCategory::protocol,
                     "provider requested tool execution, which begins in M2",
                     issue.turn_id, issue.attempt),
        issue.turn_id, config_.api_key);
  }
  return machine.apply(ModelCompleted{.response = std::move(response)});
}

TransitionResult
WorkerActor::wait_for_retry(TurnMachine& machine, const ScheduleRetryWake& wake,
                            const std::shared_ptr<std::atomic<bool>>& cancelled,
                            const std::stop_token& stopped) {
  while (!stopped.stop_requested()) {
    if (cancelled->load(std::memory_order_acquire)) {
      return machine.apply(CancelTurn{});
    }
    auto command = commands_->wait_pop_until(stopped, wake.deadline);
    if (!command) {
      break;
    }
    accept_command(std::move(*command));
  }
  if (stopped.stop_requested() || cancelled->load(std::memory_order_acquire)) {
    return machine.apply(CancelTurn{});
  }
  return machine.apply(RetryWake{
      .observed_at = std::chrono::steady_clock::now(),
  });
}

Status
WorkerActor::publish_stream_events(TurnMachine& machine,
                                   const std::vector<ProviderEvent>& provider_events,
                                   std::optional<ModelResponse>& completed_response,
                                   const bool semantic_output_consumed) {
  if (semantic_output_consumed && machine.phase() == MachinePhase::awaiting_model) {
    const auto transition = machine.apply(ModelSemanticOutput{});
    if (transition.status != TransitionStatus::applied) {
      return std::unexpected(worker_error(
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
      return std::unexpected(
          worker_error(ErrorCategory::protocol,
                       "provider stream emitted more than one completion", TurnId{}));
    }
    completed_response = completed->response;
    return {};
  }
  // The provider seam preserves an ignored event's name for debug inspection.
  // M1 has no public logging surface, so the worker intentionally consumes it.
  assert(std::holds_alternative<ProviderIgnoredEvent>(event));
  return {};
}

Status WorkerActor::publish_command(const MachineCommand& command) {
  const auto payload_limit =
      config_.limits.max_queued_event_bytes_per_turn - terminal_event_reserve;
  if (const auto* delta = std::get_if<PublishTextDelta>(&command)) {
    if (!events_->push(TextDeltaEvent{.turn_id = delta->turn_id, .text = delta->text},
                       payload_limit)) {
      return std::unexpected(
          worker_error(ErrorCategory::resource_limit,
                       "turn events exceed the configured queue limit", delta->turn_id,
                       delta->attempt));
    }
    return {};
  }
  if (const auto* completion = std::get_if<CommitCompletion>(&command)) {
    if (!events_->push(
            CompletionEvent{
                .turn_id = completion->turn_id,
                .response = completion->response,
                .attempt_count = completion->attempt_count,
            },
            payload_limit)) {
      return std::unexpected(
          worker_error(ErrorCategory::resource_limit,
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
        .error = worker_error(ErrorCategory::invalid_state,
                              "worker could not process the accepted turn", turn_id),
    });
  } catch (...) {
    // Allocation failure is outside Scry's semantic-failure contract. The
    // thread boundary still never permits an exception to escape.
    return;
  }
}

} // namespace scry::detail
