#pragma once

#include "core/backend.hpp"
#include "core/provider.hpp"
#include "machine/turn_machine.hpp"
#include "runtime/queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <scry/config.hpp>
#include <scry/unique_function.hpp>
#include <stop_token>

namespace scry::detail {

// The worker's only two reads of real time. Both are empty in production,
// meaning the real steady clock and CommandQueue::wait_pop_until; tests fill
// them in so backoff and Retry-After scheduling are assertable to the
// millisecond without waiting on a wall clock.
struct WorkerTimeSource {
  UniqueFunction<MachineTimePoint()> now{};
  UniqueFunction<std::optional<WorkerCommand>(CommandQueue&, const std::stop_token&,
                                              MachineTimePoint)>
      wait_until{};
};

// The rest of what the worker would otherwise read from its surroundings, in
// one struct so the constructor stays within the six-argument limit. In
// production the seed is randomized per Harness; tests pin it to make jitter
// reproducible.
struct WorkerEnvironment {
  std::uint64_t retry_jitter_seed{};
  WorkerTimeSource time{};
};

class WorkerActor final {
public:
  WorkerActor(Config config, std::unique_ptr<ModelBackend> backend,
              std::shared_ptr<CommandQueue> commands,
              std::shared_ptr<EventQueue> events, WorkerEnvironment environment = {});

  void run(const std::stop_token& stopped) noexcept;

private:
  void accept_command(WorkerCommand command);
  void process_turn(SendTurnCommand&& command, const std::stop_token& stopped);
  void process_machine_command(TurnMachine& machine, MachineCommand command,
                               const SendTurnCommand& turn,
                               const std::stop_token& stopped,
                               std::deque<MachineCommand>& pending_commands);
  [[nodiscard]] TransitionResult failed_attempt(TurnMachine& machine, Error error,
                                                TurnId turn_id);
  [[nodiscard]] TransitionResult
  perform_attempt(TurnMachine& machine, IssueModelRequest issue,
                  const std::shared_ptr<std::atomic<bool>>& cancelled,
                  const std::stop_token& stopped);
  [[nodiscard]] TransitionResult
  wait_for_retry(TurnMachine& machine, const ScheduleRetryWake& wake,
                 const std::shared_ptr<std::atomic<bool>>& cancelled,
                 const std::stop_token& stopped);
  [[nodiscard]] TransitionResult wait_for_tool(TurnMachine& machine,
                                               const SendTurnCommand& turn,
                                               const std::stop_token& stopped);
  [[nodiscard]] std::optional<TransitionResult>
  handle_tool_wait_command(TurnMachine& machine, WorkerCommand command,
                           const SendTurnCommand& turn);
  [[nodiscard]] TransitionResult complete_attempt(TurnMachine& machine,
                                                  ModelResponse response);
  [[nodiscard]] Status publish_provider_event(TurnMachine& machine,
                                              ProviderEvent event);
  [[nodiscard]] Status publish_tool_batch(PublishToolCall first,
                                          std::deque<MachineCommand>& pending_commands);
  [[nodiscard]] Status publish_text_delta(PublishTextDelta delta);
  void publish_terminal_command(MachineCommand command);
  void publish_terminal_event(WorkerEvent event);
  void publish_unhandled_failure(TurnId turn_id) noexcept;
  [[nodiscard]] std::size_t streamed_event_limit() const noexcept;

  Config config_{};
  std::unique_ptr<ModelBackend> backend_{};
  std::shared_ptr<CommandQueue> commands_{};
  std::shared_ptr<EventQueue> events_{};
  std::uint64_t retry_jitter_seed_{};
  WorkerTimeSource time_{};
  std::deque<SendTurnCommand> pending_{};
};

} // namespace scry::detail
