#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <scry/turn_id.hpp>
#include <scry/unique_function.hpp>
#include <string>
#include <string_view>

namespace scry {

/// Provider-neutral reason a model response stopped.
enum class FinishReason : std::uint8_t {
  /// The model completed normally.
  completed,
  /// The configured or provider limit truncated the response.
  length,
  /// The model requested one or more tools.
  tool_use,
  /// The provider supplied no recognized finish reason.
  unknown,
};

/// Token usage reported by the provider for a completed turn.
struct Usage {
  /// Tokens consumed by provider input.
  std::uint64_t input_tokens{};
  /// Tokens emitted by provider output.
  std::uint64_t output_tokens{};
};

/// Informational view of a tool invocation accepted by the agentic loop.
///
/// Callback arguments are borrowed for the duration of the callback. Copy any fields
/// that must outlive it.
struct ToolCall {
  /// Turn that owns this call.
  TurnId turn_id{};
  /// Provider-assigned call identifier, unique within the turn.
  std::string id{};
  /// Registered tool name.
  std::string name{};
  /// Canonical JSON object passed to the tool handler.
  Json arguments{};
};

/// Final successful result of an accepted turn.
///
/// Harness::send() delivers a Result<Completion> by value to on_finished, so that
/// callback owns its argument and may move or copy fields that must outlive it.
struct Completion {
  /// Completed turn.
  TurnId turn_id{};
  /// Final assistant text.
  std::string text{};
  /// Provider-neutral finish reason.
  FinishReason finish_reason{FinishReason::unknown};
  /// Provider-reported usage accumulated for the turn.
  Usage usage{};
  /// Total model-request attempts across the turn.
  std::uint32_t attempt_count{};
  /// Sanitized provider request identifier for the completion.
  std::string provider_request_id{};
};

/// Limits one Harness::update() pump invocation.
struct UpdateOptions {
  /// Optional soft deadline checked between callbacks.
  ///
  /// One callback or tool handler is never preempted and may overrun this budget.
  std::optional<std::chrono::microseconds> time_budget{};
  /// Maximum callbacks delivered by this pump invocation.
  std::size_t max_callbacks{std::numeric_limits<std::size_t>::max()};
};

/// Summary returned from Harness::update().
struct UpdateStats {
  /// Number of callbacks delivered during this invocation.
  std::size_t callbacks_delivered{};
  /// Number of events still queued after this invocation.
  std::size_t events_remaining{};
  /// Whether a time/callback budget was exhausted or a reentrant update was rejected.
  ///
  /// A rejected reentrant update delivers no callbacks and leaves queued events
  /// untouched, so callbacks_delivered is zero and this flag is true.
  bool budget_exhausted{false};
};

/// Callback for a coalesced fragment of streamed assistant text.
///
/// The string view is borrowed only for the callback invocation.
using TextDeltaCallback = UniqueFunction<void(std::string_view)>;

/// Callback observing an accepted tool call after its result is applied.
using ToolCallCallback = UniqueFunction<void(const ToolCall&)>;

/// Callbacks delivered on the Harness::update() thread for one turn.
///
/// Every member is optional; an empty member simply means no delivery of that kind.
/// Callbacks are supplied to Harness::send() and are attached from the moment the turn
/// is accepted, so no event can precede them.
struct TurnCallbacks {
  /// Observes coalesced fragments of streamed assistant text.
  TextDeltaCallback on_text_delta{};
  /// Observes accepted tool calls after their results are applied.
  ToolCallCallback on_tool_call{};
  /// When non-empty, invoked exactly once per accepted turn unless Harness destruction
  /// begins first.
  ///
  /// Delivery happens on the Harness::update() thread and carries the Completion on
  /// success, the terminal Error on failure, and on cancellation an Error whose
  /// category is ErrorCategory::cancelled and whose turn_id identifies the turn.
  /// Dropping the Turn handle does not suppress it. An empty callback is legal and
  /// requests no terminal delivery; the turn still terminates and commits or rolls back
  /// its Conversation as usual.
  UniqueFunction<void(Result<Completion>)> on_finished{};
};

} // namespace scry
