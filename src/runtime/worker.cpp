#include "runtime/worker.hpp"

#include "core/retry.hpp"
#include "protocol/sse.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

constexpr std::size_t terminal_event_reserve = 512;

[[nodiscard]] Error worker_error(const ErrorCategory category, std::string message,
                                 const TurnId turn_id,
                                 const std::uint32_t attempt = 0) {
  return Error{
      .category = category,
      .attempt = attempt,
      .message = std::move(message),
      .turn_id = turn_id,
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

void append_commands(std::deque<MachineCommand>& destination,
                     TransitionResult transition) {
  for (auto& command : transition.commands) {
    destination.push_back(std::move(command));
  }
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
                                              const std::string_view secret,
                                              const std::uint64_t retry_jitter_seed,
                                              const MachineTimePoint observed_at) {
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
      .observed_at = observed_at,
      .retry_after = retry_after,
      .jitter_sample = retry_jitter_sample(retry_jitter_seed, turn_id, attempt),
  });
}

struct AttemptLimits final {
  std::size_t maximum_event_bytes{};
  std::size_t maximum_tool_arguments_bytes{};
};

} // namespace

WorkerActor::WorkerActor(Config config, std::unique_ptr<ProviderAdapter> provider,
                         std::unique_ptr<Transport> transport,
                         std::shared_ptr<CommandQueue> commands,
                         std::shared_ptr<EventQueue> events,
                         WorkerEnvironment environment)
    : config_(std::move(config)), provider_(std::move(provider)),
      transport_(std::move(transport)), commands_(std::move(commands)),
      events_(std::move(events)), retry_jitter_seed_(environment.retry_jitter_seed),
      time_(std::move(environment.time)) {
  // An empty member means production: the real steady clock and the real
  // deadline wait on the command queue.
  if (!time_.now) {
    time_.now = [] { return std::chrono::steady_clock::now(); };
  }
  if (!time_.wait_until) {
    time_.wait_until = [](CommandQueue& queue, const std::stop_token& stopped,
                          const MachineTimePoint deadline) {
      return queue.wait_pop_until(stopped, deadline);
    };
  }
}

struct WorkerActor::AttemptState {
  explicit AttemptState(const AttemptLimits& limits)
      : parser(limits.maximum_event_bytes),
        decode{.max_tool_arguments_bytes = limits.maximum_tool_arguments_bytes} {}

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
  if (std::holds_alternative<ToolResultCommand>(command)) {
    // A tool result is meaningful only while its turn owns the serialized
    // worker slot. Results arriving after terminal cancellation are stale.
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
  TurnMachine machine{
      command.turn_id,
      std::move(command.request),
      config_.retry,
      ToolLoopPolicy{
          .max_rounds = config_.max_tool_rounds,
          .max_argument_bytes = config_.limits.max_tool_arguments_bytes,
          .max_exchange_bytes = command.max_exchange_bytes,
      },
  };
  std::deque<MachineCommand> machine_commands;
  if (command.cancelled->load(std::memory_order_acquire)) {
    append_commands(machine_commands, machine.apply(CancelTurn{}));
  } else {
    append_commands(machine_commands, machine.apply(BeginTurn{
                                          .observed_at = time_.now(),
                                      }));
  }

  while (!stopped.stop_requested()) {
    if (machine_commands.empty()) {
      if (machine.phase() == MachinePhase::awaiting_tool) {
        append_commands(machine_commands, wait_for_tool(machine, command, stopped));
        continue;
      }
      return;
    }
    auto next = std::move(machine_commands.front());
    machine_commands.pop_front();
    if (!process_machine_command(machine, std::move(next), command, stopped,
                                 machine_commands)) {
      return;
    }
  }
}

bool WorkerActor::process_machine_command(
    TurnMachine& machine, MachineCommand command, const SendTurnCommand& turn,
    const std::stop_token& stopped, std::deque<MachineCommand>& pending_commands) {
  if (const auto* issue = std::get_if<IssueModelRequest>(&command)) {
    if (turn.cancelled->load(std::memory_order_acquire)) {
      append_commands(pending_commands, machine.apply(CancelTurn{}));
    } else {
      append_commands(pending_commands,
                      perform_attempt(machine, *issue, turn.cancelled, stopped));
    }
    return true;
  }
  if (const auto* wake = std::get_if<ScheduleRetryWake>(&command)) {
    append_commands(pending_commands,
                    wait_for_retry(machine, *wake, turn.cancelled, stopped));
    return true;
  }
  if (auto* tool = std::get_if<PublishToolCall>(&command)) {
    auto published = publish_tool_batch(std::move(*tool), pending_commands);
    if (!published) {
      pending_commands.clear();
      append_commands(pending_commands, machine.apply(ToolExecutionFailed{
                                            .error = std::move(published.error()),
                                        }));
    }
    return true;
  }
  auto published = publish_command(std::move(command));
  if (published) {
    return true;
  }
  if (machine.phase() == MachinePhase::terminal) {
    auto error = worker_error(ErrorCategory::resource_limit,
                              "turn events exceed the configured queue limit",
                              turn.turn_id, machine.attempt_count());
    publish_terminal_event(
        ErrorEvent{.turn_id = turn.turn_id, .error = std::move(error)});
    return false;
  }
  if (machine.phase() == MachinePhase::awaiting_tool) {
    append_commands(pending_commands, machine.apply(ToolExecutionFailed{
                                          .error = std::move(published.error()),
                                      }));
  } else {
    append_commands(pending_commands,
                    failed_attempt(machine, std::move(published.error()), turn.turn_id,
                                   config_.api_key, retry_jitter_seed_, time_.now()));
  }
  return true;
}

TransitionResult
WorkerActor::perform_attempt(TurnMachine& machine, const IssueModelRequest& issue,
                             const std::shared_ptr<std::atomic<bool>>& cancelled,
                             const std::stop_token& stopped) {
  auto request = provider_->make_request(config_, *issue.request);
  if (!request) {
    return failed_attempt(machine, std::move(request.error()), issue.turn_id,
                          config_.api_key, retry_jitter_seed_, time_.now());
  }

  AttemptState state{AttemptLimits{
      .maximum_event_bytes = config_.limits.max_sse_event_bytes,
      .maximum_tool_arguments_bytes = config_.limits.max_tool_arguments_bytes,
  }};
  BodyChunkSink body_sink{
      [this, &machine, &state](const std::string_view chunk) -> Status {
        return consume_stream_chunk(machine, state, chunk);
      }};

  auto result = transport_->perform(*request, stopped, *cancelled, body_sink);
  if (!result) {
    return failed_attempt(machine, std::move(result.error()), issue.turn_id,
                          config_.api_key, retry_jitter_seed_, time_.now());
  }
  auto response = finish_stream(machine, state);
  if (!response) {
    return failed_attempt(machine, std::move(response.error()), issue.turn_id,
                          config_.api_key, retry_jitter_seed_, time_.now());
  }
  return complete_attempt(machine, std::move(*response), *result);
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
    auto status =
        publish_stream_events(machine, std::move(*provider_events), state.completed,
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
                                               const TransportResult& result) {
  if (response.provider_request_id.empty()) {
    response.provider_request_id = result.provider_request_id;
  }
  if (!config_.api_key.empty() &&
      response.provider_request_id.find(config_.api_key) != std::string::npos) {
    response.provider_request_id.clear();
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
    auto command = time_.wait_until(*commands_, stopped, wake.deadline);
    if (!command) {
      break;
    }
    accept_command(std::move(*command));
  }
  if (stopped.stop_requested() || cancelled->load(std::memory_order_acquire)) {
    return machine.apply(CancelTurn{});
  }
  return machine.apply(RetryWake{
      .observed_at = time_.now(),
  });
}

TransitionResult WorkerActor::wait_for_tool(TurnMachine& machine,
                                            const SendTurnCommand& turn,
                                            const std::stop_token& stopped) {
  while (!stopped.stop_requested()) {
    if (turn.cancelled->load(std::memory_order_acquire)) {
      return machine.apply(CancelTurn{});
    }
    auto command = commands_->wait_pop(stopped);
    if (!command) {
      break;
    }
    auto transition = handle_tool_wait_command(machine, std::move(*command), turn);
    if (transition) {
      return std::move(*transition);
    }
  }
  return machine.apply(CancelTurn{});
}

std::optional<TransitionResult>
WorkerActor::handle_tool_wait_command(TurnMachine& machine, WorkerCommand command,
                                      const SendTurnCommand& turn) {
  if (auto* result = std::get_if<ToolResultCommand>(&command)) {
    if (result->turn_id != turn.turn_id) {
      return std::nullopt;
    }
    if (!result->result) {
      return machine.apply(
          ToolExecutionFailed{.error = std::move(result->result.error())});
    }
    return machine.apply(ToolResultReady{
        .result = std::move(*result->result),
        .observed_at = time_.now(),
    });
  }
  if (const auto* cancel = std::get_if<CancelTurnCommand>(&command);
      cancel != nullptr && cancel->turn_id == turn.turn_id) {
    return machine.apply(CancelTurn{});
  }
  accept_command(std::move(command));
  return std::nullopt;
}

Status
WorkerActor::publish_stream_events(TurnMachine& machine,
                                   std::vector<ProviderEvent> provider_events,
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
  for (auto& event : provider_events) {
    auto status = publish_provider_event(machine, std::move(event), completed_response);
    if (!status) {
      return status;
    }
  }
  return {};
}

Status
WorkerActor::publish_provider_event(TurnMachine& machine, ProviderEvent event,
                                    std::optional<ModelResponse>& completed_response) {
  if (auto* text = std::get_if<ProviderTextDelta>(&event)) {
    auto transition = machine.apply(ModelTextDelta{.text = std::move(text->text)});
    for (auto& command : transition.commands) {
      auto status = publish_command(std::move(command));
      if (!status) {
        return status;
      }
    }
    return {};
  }
  if (auto* completed = std::get_if<ProviderCompleted>(&event)) {
    if (completed_response) {
      return std::unexpected(
          worker_error(ErrorCategory::protocol,
                       "provider stream emitted more than one completion", TurnId{}));
    }
    completed_response = std::move(completed->response);
    return {};
  }
  // The provider seam preserves an ignored event's name for debug inspection.
  // Scry has no public logging surface, so the worker consumes the marker.
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
    return std::unexpected(
        worker_error(ErrorCategory::resource_limit,
                     "tool-call batch exceeds the configured queue limit", turn_id));
  }
  return {};
}

Status WorkerActor::publish_command(MachineCommand command) {
  const auto payload_limit =
      config_.limits.max_queued_event_bytes_per_turn - terminal_event_reserve;
  if (auto* delta = std::get_if<PublishTextDelta>(&command)) {
    // The rejection path below reads only the scalar correlation fields, so
    // handing the text to the queue costs nothing on failure.
    if (!events_->push(
            TextDeltaEvent{.turn_id = delta->turn_id, .text = std::move(delta->text)},
            payload_limit)) {
      return std::unexpected(
          worker_error(ErrorCategory::resource_limit,
                       "turn events exceed the configured queue limit", delta->turn_id,
                       delta->attempt));
    }
    return {};
  }
  if (auto* completion = std::get_if<CommitCompletion>(&command)) {
    if (!events_->push(
            CompletionEvent{
                .turn_id = completion->turn_id,
                .exchange = std::move(completion->exchange),
                .finish_reason = completion->finish_reason,
                .usage = completion->usage,
                .attempt_count = completion->attempt_count,
                .provider_request_id = std::move(completion->provider_request_id),
            },
            payload_limit)) {
      return std::unexpected(
          worker_error(ErrorCategory::resource_limit,
                       "turn events exceed the configured queue limit",
                       completion->turn_id, completion->attempt_count));
    }
    return {};
  }
  if (auto* error = std::get_if<PublishError>(&command)) {
    const auto turn_id = error->error.turn_id.value_or(TurnId{});
    publish_terminal_event(
        ErrorEvent{.turn_id = turn_id, .error = std::move(error->error)});
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
