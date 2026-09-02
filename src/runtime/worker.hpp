/// @file
/// @brief Worker-thread actor that drives transport, provider decoding, and
/// TurnMachine.
///
/// WorkerActor exclusively owns all mutable network, stream-parser, provider-decode,
/// and agentic-machine state. It receives commands through CommandQueue and publishes
/// neutral events through EventQueue; no callback, tool handler, or Conversation
/// mutation occurs on this side of the boundary.

#pragma once

#include "core/provider.hpp"
#include "core/transport.hpp"
#include "machine/turn_machine.hpp"
#include "protocol/sse.hpp"
#include "runtime/queue.hpp"

#include <deque>
#include <memory>
#include <optional>
#include <scry/config.hpp>
#include <stop_token>
#include <string_view>
#include <vector>

namespace scry::detail {

/// Single-threaded actor driving accepted turns through the sans-I/O machine.
///
/// One Harness owns one actor on a std::jthread. Accepted turns are scheduled FIFO and
/// exactly one turn owns the serialized transport slot, including while it waits for an
/// app-thread tool result or retry deadline. Harness shutdown arrives through the
/// worker stop token; per-turn cancellation uses each SendTurnCommand's separate atomic
/// flag.
class WorkerActor final {
public:
  /// Takes exclusive ownership of worker-side runtime dependencies.
  ///
  /// @param config Already validated provider, retry, timeout, and resource policy.
  /// @param provider Stateless wire-format adapter selected for this Harness.
  /// @param transport Blocking transport implementation used only by this actor.
  /// @param commands Shared app-to-worker command FIFO.
  /// @param events Shared bounded worker-to-pump event FIFO.
  WorkerActor(Config config, std::unique_ptr<ProviderAdapter> provider,
              std::unique_ptr<Transport> transport,
              std::shared_ptr<CommandQueue> commands,
              std::shared_ptr<EventQueue> events);

  /// Runs the actor loop until Harness shutdown is requested.
  ///
  /// Unexpected failures while processing an accepted turn are caught and converted to
  /// a terminal event so no exception crosses the thread boundary.
  ///
  /// @param stopped Harness-lifetime stop token owned by the worker jthread.
  void run(const std::stop_token& stopped) noexcept;

private:
  /// Per-model-attempt SSE and dialect-specific decode state.
  ///
  /// A fresh instance is created for every request attempt so parser fragments and
  /// provider lifecycle state cannot leak across retries, turns, or Harness dialects.
  struct AttemptState;

  /// Routes an out-of-band command into actor-owned scheduling state.
  ///
  /// Send commands join the FIFO, cancellation removes matching queued turns, and stale
  /// tool results outside the active tool wait are ignored.
  ///
  /// @param command Command transferred from the shared queue.
  void accept_command(WorkerCommand command);
  /// Drives one accepted turn until it publishes a terminal outcome or shutdown wins.
  ///
  /// @param command FIFO send command whose request is consumed by the machine.
  /// @param stopped Harness-shutdown signal.
  void process_turn(SendTurnCommand&& command, const std::stop_token& stopped);
  /// Executes one command emitted by TurnMachine.
  ///
  /// New machine transitions are appended to @p pending_commands. Provider batches are
  /// admitted atomically, queue-pressure failures are routed back through the machine
  /// while it is recoverable, and a terminal publication failure ends local processing.
  ///
  /// @param machine Sans-I/O state machine owning the turn protocol.
  /// @param command Machine command transferred for execution.
  /// @param turn Original accepted-turn resources and cancellation flag.
  /// @param stopped Harness-shutdown signal.
  /// @param pending_commands Worklist receiving commands from resulting transitions.
  /// @return true to continue this turn; false after final local publication.
  [[nodiscard]] bool
  process_machine_command(TurnMachine& machine, MachineCommand command,
                          const SendTurnCommand& turn, const std::stop_token& stopped,
                          std::deque<MachineCommand>& pending_commands);
  /// Performs one blocking streamed model request and feeds its outcome to the machine.
  ///
  /// @param machine Active turn machine.
  /// @param issue Request snapshot and attempt metadata emitted by the machine.
  /// @param cancelled Per-turn cancellation atomic checked by transport progress.
  /// @param stopped Harness-shutdown signal checked independently by transport.
  /// @return Commands and status produced by completion or classified attempt failure.
  [[nodiscard]] TransitionResult
  perform_attempt(TurnMachine& machine, const IssueModelRequest& issue,
                  const std::shared_ptr<std::atomic<bool>>& cancelled,
                  const std::stop_token& stopped);
  /// Waits cooperatively for an injected retry deadline while servicing other commands.
  ///
  /// Newly accepted turns remain FIFO-pending; cancellation for the active turn
  /// interrupts the wait. The machine, not this driver, owns retry eligibility and
  /// backoff policy.
  ///
  /// @param machine Active turn machine.
  /// @param wake Retry deadline emitted by the machine.
  /// @param cancelled Per-turn cancellation signal.
  /// @param stopped Harness-shutdown signal.
  /// @return Transition produced by RetryWake or cancellation.
  [[nodiscard]] TransitionResult
  wait_for_retry(TurnMachine& machine, const ScheduleRetryWake& wake,
                 const std::shared_ptr<std::atomic<bool>>& cancelled,
                 const std::stop_token& stopped);
  /// Waits for the next ordered app-thread tool result while retaining the worker slot.
  ///
  /// Commands for other turns are routed into pending state without letting them bypass
  /// the active turn.
  ///
  /// @param machine Active turn machine in MachinePhase::awaiting_tool.
  /// @param turn Accepted-turn identity and cancellation flag.
  /// @param stopped Harness-shutdown signal.
  /// @return Transition produced by a matching result or cancellation.
  [[nodiscard]] TransitionResult wait_for_tool(TurnMachine& machine,
                                               const SendTurnCommand& turn,
                                               const std::stop_token& stopped);
  /// Classifies one command observed during the active turn's tool wait.
  ///
  /// @param machine Active turn machine.
  /// @param command Newly dequeued command.
  /// @param turn Turn currently owning the serialized worker slot.
  /// @return Matching transition, or std::nullopt after routing another command.
  [[nodiscard]] std::optional<TransitionResult>
  handle_tool_wait_command(TurnMachine& machine, WorkerCommand command,
                           const SendTurnCommand& turn);
  /// Incrementally parses one transport body chunk into provider events.
  ///
  /// @param machine Active turn machine receiving semantic events.
  /// @param state Attempt-local SSE and provider decode state.
  /// @param chunk Arbitrarily split bytes delivered by the transport.
  /// @return Success, or a bounded protocol/resource error.
  [[nodiscard]] Status consume_stream_chunk(TurnMachine& machine, AttemptState& state,
                                            std::string_view chunk);
  /// Decodes and publishes a batch of complete SSE events in stream order.
  ///
  /// @param machine Active turn machine.
  /// @param state Attempt-local provider decoder and completion slot.
  /// @param events Parsed SSE events to consume exactly once.
  /// @return Success, or the first provider/machine/publication failure.
  [[nodiscard]] Status consume_sse_events(TurnMachine& machine, AttemptState& state,
                                          const std::vector<SseEvent>& events);
  /// Finalizes SSE framing and requires exactly one provider completion.
  ///
  /// @param machine Active turn machine.
  /// @param state Attempt state whose parser remainder and completion are consumed.
  /// @return Completed neutral response, or a protocol/publication error.
  [[nodiscard]] Result<ModelResponse> finish_stream(TurnMachine& machine,
                                                    AttemptState& state);
  /// Adds sanitized transport correlation and applies model completion to the machine.
  ///
  /// @param machine Active turn machine.
  /// @param response Provider-decoded response transferred into the transition.
  /// @param result Transport metadata for fallback request correlation.
  /// @return Machine transition for the completed response.
  [[nodiscard]] TransitionResult complete_attempt(TurnMachine& machine,
                                                  ModelResponse response,
                                                  const TransportResult& result);
  /// Publishes decoded provider events and advances the streaming phase when necessary.
  ///
  /// Provider events and resulting machine commands are consumed exactly once, allowing
  /// streamed text to move all the way to EventQueue without per-hop copies.
  ///
  /// @param machine Active turn machine.
  /// @param provider_events Ordered decoded events transferred for publication.
  /// @param completed_response Attempt-local slot for the sole completion event.
  /// @param semantic_output_consumed Whether semantic output was consumed.
  /// @return Success, or a state/publication error.
  [[nodiscard]] Status publish_stream_events(
      TurnMachine& machine, std::vector<ProviderEvent> provider_events,
      std::optional<ModelResponse>& completed_response, bool semantic_output_consumed);
  /// Converts one provider event into machine input or completion state.
  ///
  /// Ignored optional provider events are consumed only by internal diagnostics;
  /// required unmappable content is rejected by the adapter before reaching this
  /// method.
  ///
  /// @param machine Active turn machine.
  /// @param event Decoded provider event transferred for handling.
  /// @param completed_response Attempt-local slot enforcing one completion.
  /// @return Success, or a duplicate-completion/machine/publication error.
  [[nodiscard]] Status
  publish_provider_event(TurnMachine& machine, ProviderEvent event,
                         std::optional<ModelResponse>& completed_response);
  /// Atomically publishes a contiguous machine-emitted tool-call batch.
  ///
  /// Consecutive PublishToolCall commands for @p first's turn are removed from the
  /// worklist and inserted into EventQueue as one all-or-nothing batch.
  ///
  /// @param first First call command transferred into the event batch.
  /// @param pending_commands Machine worklist from which the batch suffix is consumed.
  /// @return Success, or a resource-limit error when the whole batch cannot fit.
  [[nodiscard]] Status publish_tool_batch(PublishToolCall first,
                                          std::deque<MachineCommand>& pending_commands);
  /// Publishes one non-batch machine command as its worker event representation.
  ///
  /// @param command Command transferred into EventQueue or a terminal publisher.
  /// @return Success, or a queue resource-limit error when a text delta or ordinary
  /// completion cannot fit.
  [[nodiscard]] Status publish_command(MachineCommand command);
  /// Publishes a bounded error or cancellation using the queue's reserved capacity.
  ///
  /// Ordinary successful completions use publish_command() and its normal payload
  /// ceiling. This fallback path is reserved for the bounded ErrorEvent and
  /// CancelledEvent shapes that must remain publishable after ordinary capacity is
  /// exhausted.
  ///
  /// @param event Error or cancellation transferred to the pump.
  void publish_terminal_event(WorkerEvent event);
  /// Best-effort terminalizes a turn after an unexpected worker exception.
  ///
  /// No exception is permitted to escape the actor thread; allocation failure during
  /// this last-resort path is intentionally swallowed because OOM is outside Scry's
  /// semantic failure contract.
  ///
  /// @param turn_id Accepted turn being processed when the exception escaped.
  void publish_unhandled_failure(TurnId turn_id) noexcept;

  /// Immutable validated Harness policy copied into worker ownership.
  Config config_{};
  /// Harness-selected provider wire adapter, used only on the worker thread.
  std::unique_ptr<ProviderAdapter> provider_{};
  /// Blocking transport implementation, used only on the worker thread.
  std::unique_ptr<Transport> transport_{};
  /// Shared command synchronization point from the app/pump side.
  std::shared_ptr<CommandQueue> commands_{};
  /// Shared bounded event synchronization point to the pump side.
  std::shared_ptr<EventQueue> events_{};
  /// Accepted turns waiting in FIFO order for the serialized worker slot.
  std::deque<SendTurnCommand> pending_{};
};

} // namespace scry::detail
