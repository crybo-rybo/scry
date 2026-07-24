#include "runtime/worker.hpp"

#include "protocol/sse.hpp"
#include "runtime/tool_dispatch.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

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

struct AttemptLimits final {
  std::size_t maximum_event_bytes{};
  std::size_t maximum_tool_arguments_bytes{};
};

} // namespace

WorkerActor::WorkerActor(Config config, std::unique_ptr<ProviderAdapter> provider,
                         std::unique_ptr<Transport> transport,
                         std::shared_ptr<CommandQueue> commands,
                         std::shared_ptr<EventQueue> events)
    : config_(std::move(config)), provider_(std::move(provider)),
      transport_(std::move(transport)), commands_(std::move(commands)),
      events_(std::move(events)) {}

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
  if (auto* registration = std::get_if<RegisterWorkerToolCommand>(&command)) {
    register_worker_tool(std::move(*registration));
    return;
  }
  if (auto* send = std::get_if<SendTurnCommand>(&command)) {
    pending_.push_back(std::move(*send));
    return;
  }
  if (std::holds_alternative<ToolResultCommand>(command) ||
      std::holds_alternative<ExecuteWorkerToolCommand>(command)) {
    // Tool work is meaningful only while its turn owns the serialized worker
    // slot. Values that arrive after terminal cancellation are stale.
    return;
  }
  const auto turn_id = std::get<CancelTurnCommand>(command).turn_id;
  const auto found = std::ranges::find(pending_, turn_id, &SendTurnCommand::turn_id);
  if (found != pending_.end()) {
    pending_.erase(found);
    publish_terminal_event(CancelledEvent{.turn_id = turn_id});
  }
}

void WorkerActor::register_worker_tool(RegisterWorkerToolCommand command) {
  const auto duplicate =
      std::ranges::any_of(worker_tools_, [&command](const WorkerTool& tool) {
        return tool.name == command.name;
      });
  assert(!duplicate);
  if (duplicate) {
    return;
  }
  worker_tools_.push_back(WorkerTool{
      .name = std::move(command.name),
      .handler = std::move(command.handler),
  });
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
    append_commands(machine_commands,
                    machine.apply(BeginTurn{
                        .observed_at = std::chrono::steady_clock::now(),
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
                                   config_.api_key));
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
                          config_.api_key);
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
                          config_.api_key);
  }
  auto response = finish_stream(machine, state);
  if (!response) {
    return failed_attempt(machine, std::move(response.error()), issue.turn_id,
                          config_.api_key);
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
    auto transition =
        handle_tool_wait_command(machine, std::move(*command), turn, stopped);
    if (transition) {
      return std::move(*transition);
    }
  }
  return machine.apply(CancelTurn{});
}

std::optional<TransitionResult>
WorkerActor::handle_tool_wait_command(TurnMachine& machine, WorkerCommand command,
                                      const SendTurnCommand& turn,
                                      const std::stop_token& stopped) {
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
        .observed_at = std::chrono::steady_clock::now(),
    });
  }
  if (auto* execute = std::get_if<ExecuteWorkerToolCommand>(&command)) {
    if (execute->turn_id != turn.turn_id) {
      return std::nullopt;
    }
    return execute_worker_tool(machine, std::move(*execute), turn, stopped);
  }
  if (const auto* cancel = std::get_if<CancelTurnCommand>(&command);
      cancel != nullptr && cancel->turn_id == turn.turn_id) {
    return machine.apply(CancelTurn{});
  }
  accept_command(std::move(command));
  return std::nullopt;
}

TransitionResult WorkerActor::execute_worker_tool(TurnMachine& machine,
                                                  ExecuteWorkerToolCommand command,
                                                  const SendTurnCommand& turn,
                                                  const std::stop_token& stopped) {
  if (turn.cancelled->load(std::memory_order_acquire)) {
    return machine.apply(CancelTurn{});
  }
  const auto accepted = std::ranges::find(turn.worker_tool_names, command.call.name) !=
                        turn.worker_tool_names.end();
  auto* handler = accepted ? find_worker_handler(command.call.name) : nullptr;
  if (handler == nullptr) {
    return machine.apply(ToolExecutionFailed{
        .error = worker_error(ErrorCategory::invalid_state,
                              "worker tool is unavailable for the accepted turn",
                              turn.turn_id, machine.attempt_count()),
    });
  }

  auto result = dispatch_tool_handler(*handler, command.call,
                                      config_.limits.max_tool_result_bytes);
  if (stopped.stop_requested() || turn.cancelled->load(std::memory_order_acquire)) {
    return machine.apply(CancelTurn{});
  }
  if (!result) {
    return machine.apply(ToolExecutionFailed{.error = std::move(result.error())});
  }

  const auto result_payload_bytes = content_payload_bytes(*result);
  auto transition = machine.apply(ToolResultReady{
      .result = std::move(*result),
      .observed_at = std::chrono::steady_clock::now(),
  });
  if (transition.status == TransitionStatus::applied &&
      machine.phase() != MachinePhase::terminal) {
    publish_worker_tool_accepted(turn.turn_id, std::move(command.call.id),
                                 result_payload_bytes);
  }
  return transition;
}

ToolHandler* WorkerActor::find_worker_handler(const std::string_view name) noexcept {
  const auto found = std::ranges::find(worker_tools_, name, &WorkerTool::name);
  return found == worker_tools_.end() ? nullptr : &found->handler;
}

} // namespace scry::detail
