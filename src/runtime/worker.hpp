#pragma once

#include "core/provider.hpp"
#include "core/transport.hpp"
#include "machine/turn_machine.hpp"
#include "protocol/sse.hpp"
#include "runtime/queue.hpp"

#include <deque>
#include <memory>
#include <scry/config.hpp>
#include <stop_token>

namespace scry::detail {

class WorkerActor final {
public:
  WorkerActor(Config config, std::unique_ptr<ProviderAdapter> provider,
              std::unique_ptr<Transport> transport,
              std::shared_ptr<CommandQueue> commands,
              std::shared_ptr<EventQueue> events);

  void run(const std::stop_token& stopped) noexcept;

private:
  struct AttemptState;

  void accept_command(WorkerCommand command);
  void process_turn(SendTurnCommand&& command, const std::stop_token& stopped);
  [[nodiscard]] bool
  process_machine_command(TurnMachine& machine, MachineCommand command,
                          const SendTurnCommand& turn, const std::stop_token& stopped,
                          std::deque<MachineCommand>& pending_commands);
  [[nodiscard]] TransitionResult
  perform_attempt(TurnMachine& machine, const IssueModelRequest& issue,
                  const std::shared_ptr<std::atomic<bool>>& cancelled,
                  const std::stop_token& stopped);
  [[nodiscard]] TransitionResult
  wait_for_retry(TurnMachine& machine, const ScheduleRetryWake& wake,
                 const std::shared_ptr<std::atomic<bool>>& cancelled,
                 const std::stop_token& stopped);
  [[nodiscard]] TransitionResult
  wait_for_tool(TurnMachine& machine, TurnId turn_id,
                const std::shared_ptr<std::atomic<bool>>& cancelled,
                const std::stop_token& stopped);
  [[nodiscard]] Status consume_stream_chunk(TurnMachine& machine, AttemptState& state,
                                            std::string_view chunk);
  [[nodiscard]] Status consume_sse_events(TurnMachine& machine, AttemptState& state,
                                          const std::vector<SseEvent>& events);
  [[nodiscard]] Result<ModelResponse> finish_stream(TurnMachine& machine,
                                                    AttemptState& state);
  [[nodiscard]] TransitionResult complete_attempt(TurnMachine& machine,
                                                  ModelResponse response,
                                                  const TransportResult& result);
  [[nodiscard]] Status publish_stream_events(
      TurnMachine& machine, const std::vector<ProviderEvent>& provider_events,
      std::optional<ModelResponse>& completed_response, bool semantic_output_consumed);
  [[nodiscard]] Status
  publish_provider_event(TurnMachine& machine, const ProviderEvent& event,
                         std::optional<ModelResponse>& completed_response);
  [[nodiscard]] Status publish_tool_batch(PublishToolCall first,
                                          std::deque<MachineCommand>& pending_commands);
  [[nodiscard]] Status publish_command(const MachineCommand& command);
  void publish_terminal_event(WorkerEvent event);
  void publish_unhandled_failure(TurnId turn_id) noexcept;

  Config config_{};
  std::unique_ptr<ProviderAdapter> provider_{};
  std::unique_ptr<Transport> transport_{};
  std::shared_ptr<CommandQueue> commands_{};
  std::shared_ptr<EventQueue> events_{};
  std::deque<SendTurnCommand> pending_{};
};

} // namespace scry::detail
