/// @file
/// @brief Closed command and event protocol crossing the runtime thread boundary.
///
/// The Harness pump and worker actor communicate only through these move-only value
/// variants plus the per-turn cancellation atomic carried by SendTurnCommand. Commands
/// flow app-to-worker; events flow worker-to-pump and are always correlated by TurnId.

#pragma once

#include "core/model.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>
#include <string>
#include <variant>
#include <vector>

namespace scry::detail {

/// Accepts one turn into the worker actor's FIFO schedule.
///
/// The request may share immutable committed-history and schema blocks with pump-owned
/// state. The worker may read those blocks but never mutates them.
struct SendTurnCommand {
  /// Immutable identifier assigned before the command is published.
  TurnId turn_id{};
  /// Initial neutral request, including immutable history and tool schemas.
  ModelRequest request{};
  /// Per-turn cancellation signal shared with the Turn handle and transport.
  std::shared_ptr<std::atomic<bool>> cancelled{};
  /// Bytes available for assistant/tool exchange content after admission.
  std::size_t max_exchange_bytes{std::numeric_limits<std::size_t>::max()};
};

/// Requests cancellation of a queued or currently waiting turn.
///
/// The atomic flag is authoritative for active transport cancellation. This command
/// also wakes the actor and lets it remove a turn that has not issued I/O yet.
struct CancelTurnCommand {
  /// Turn whose queued or active work should terminate cooperatively.
  TurnId turn_id{};
};

/// Returns one app-thread tool-dispatch outcome to the waiting state machine.
struct ToolResultCommand {
  /// Turn that currently owns the serialized worker slot.
  TurnId turn_id{};
  /// Canonical model-visible result, or a fatal framework dispatch error.
  Result<ToolResultBlock> result{};
};

/// Exhaustive app-to-worker message protocol.
///
/// Extending this variant requires every actor command consumer to classify the new
/// alternative, preserving an explicit concurrency boundary.
using WorkerCommand =
    std::variant<SendTurnCommand, CancelTurnCommand, ToolResultCommand>;

/// Coalescible fragment of streamed assistant text.
struct TextDeltaEvent {
  /// Accepted turn that produced the fragment.
  TurnId turn_id{};
  /// Owning text payload moved through the worker-to-pump queue.
  std::string text{};
};

/// One provider-requested call awaiting app-thread dispatch.
///
/// Every call from one provider response is published as an atomic queue batch so
/// backpressure can never expose a dispatchable prefix.
struct ToolCallEvent {
  /// Accepted turn awaiting the result.
  TurnId turn_id{};
  /// Provider-neutral call identity, tool name, and canonical arguments.
  ToolCallBlock call{};
  /// Authoritative exchange budget remaining before this result is admitted.
  std::size_t remaining_exchange_bytes{std::numeric_limits<std::size_t>::max()};
};

/// Successful terminal exchange awaiting transactional Conversation commit.
///
/// The pump moves @ref exchange into committed history, then retains only @ref text for
/// the completion callback. The text is already represented inside the exchange and is
/// deliberately excluded from event_payload_bytes() to avoid double accounting.
struct CompletionEvent {
  /// Accepted turn reaching successful terminal processing.
  TurnId turn_id{};
  /// Full assistant/tool exchange to append after the original user message.
  std::vector<Message> exchange{};
  /// Final assistant text, populated by the pump before moving @ref exchange.
  std::string text{};
  /// Provider-neutral reason the final model response stopped.
  FinishReason finish_reason{FinishReason::unknown};
  /// Aggregate provider-reported usage across the complete tool loop.
  Usage usage{};
  /// Total model-request attempts across the accepted turn.
  std::uint32_t attempt_count{};
  /// Provider correlation identifier, checked for configured secrets before delivery.
  std::string provider_request_id{};
};

/// Failed terminal outcome published by the worker.
struct ErrorEvent {
  /// Accepted turn reaching failure.
  TurnId turn_id{};
  /// Categorized, sanitized failure delivered through the terminal callback.
  Error error{};
};

/// Cancelled terminal outcome published without a separately allocated diagnostic.
///
/// The pump constructs the public ErrorCategory::cancelled value at callback delivery.
struct CancelledEvent {
  /// Accepted turn reaching cooperative cancellation.
  TurnId turn_id{};
};

/// Exhaustive worker-to-pump message protocol.
using WorkerEvent = std::variant<TextDeltaEvent, ToolCallEvent, CompletionEvent,
                                 ErrorEvent, CancelledEvent>;

/// Extracts the common correlation identity from any worker event.
///
/// @param event Event alternative to inspect.
/// @return Turn identifier carried by the active alternative.
[[nodiscard]] TurnId event_turn_id(const WorkerEvent& event) noexcept;
/// Computes dynamic payload bytes charged to the per-turn event-queue ledger.
///
/// Accounting saturates rather than overflowing. It measures retained semantic payload,
/// not allocator overhead; scalar metadata and CompletionEvent::text's duplicate view
/// are intentionally excluded.
///
/// @param event Event whose retained payload is measured.
/// @return Saturating byte count charged until the pump releases the event.
[[nodiscard]] std::size_t event_payload_bytes(const WorkerEvent& event) noexcept;

} // namespace scry::detail
