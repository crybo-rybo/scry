#pragma once

/**
 * @file events.hpp
 * @brief Provider-neutral turn results, pump controls, and application callbacks.
 *
 * Worker activity reaches application code only when Harness::update() pumps these
 * provider-neutral values. Callback ownership and borrowing are explicit so a host can
 * integrate Scry into an existing main loop without exposing provider or thread
 * details.
 */

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
///
/// Adapters map provider-specific finish values into this closed common set. The value
/// is descriptive; terminal success or failure is represented by Result<Completion>,
/// not inferred from this enum alone.
enum class FinishReason : std::uint8_t {
  /// The model completed normally.
  completed,
  /// The configured or provider limit truncated the response.
  length,
  /// The model requested one or more tools.
  tool_use,
  /// The provider supplied no recognized optional finish reason.
  unknown,
};

/// Token usage reported by the provider for a completed turn.
///
/// Counts accumulate across all model requests and tool rounds in the turn. Providers
/// that omit usage leave the corresponding values at zero.
struct Usage {
  /// Tokens consumed by provider input.
  std::uint64_t input_tokens{};
  /// Tokens emitted by provider output.
  std::uint64_t output_tokens{};
};

/// Informational view of a tool invocation accepted by the agentic loop.
///
/// ToolCallCallback observes this value only after app-thread dispatch produces and
/// queues a model-visible result, whether successful or a tool error. The observer is
/// not an authorization or execution hook; omitting it changes no loop behavior.
/// Callback arguments are borrowed for the duration of the callback. Copy any fields
/// that must outlive it.
struct ToolCall {
  /// Turn that owns this call.
  TurnId turn_id{};
  /// Provider-assigned call identifier used for at-most-once dispatch within the turn.
  std::string id{};
  /// Provider-requested tool name, which need not match a registration.
  std::string name{};
  /// Canonical JSON object passed to the tool handler.
  ///
  /// The explicit handler is responsible for validating its application-level shape.
  Json arguments{};
};

/// Final successful result of an accepted turn.
///
/// Harness::send() delivers a Result<Completion> by value to on_finished, so that
/// callback owns its argument and may move or copy fields that must outlive it. A
/// Completion means the full exchange was accepted for transactional commit; failures
/// and cancellation are instead represented by Error.
struct Completion {
  /// Completed turn.
  TurnId turn_id{};
  /// Final assistant text accumulated across the terminal model response.
  std::string text{};
  /// Provider-neutral finish reason.
  FinishReason finish_reason{FinishReason::unknown};
  /// Provider-reported usage accumulated for the turn.
  Usage usage{};
  /// Total model-request attempts across the turn.
  std::uint32_t attempt_count{};
  /// Sanitized provider request identifier for the terminal model request.
  std::string provider_request_id{};
};

/// Limits one Harness::update() pump invocation.
///
/// Both limits are cooperative scheduling controls. They never preempt a callback or
/// handler already running and do not affect worker-side network progress.
struct UpdateOptions {
  /// Optional soft deadline checked between callbacks.
  ///
  /// One callback or tool handler is never preempted and may overrun this budget.
  std::optional<std::chrono::microseconds> time_budget{};
  /// Maximum queued event deliveries processed by this pump invocation.
  ///
  /// A tool-call event counts once even though it always invokes a handler and may also
  /// invoke an observer. A value of zero performs no delivery during this invocation.
  std::size_t max_callbacks{std::numeric_limits<std::size_t>::max()};
};

/// Summary returned from Harness::update().
///
/// Statistics describe only the completed pump call; they are not persistent Harness
/// counters and should not be used as turn-completion polling state.
struct UpdateStats {
  /// Number of queued event deliveries processed during this invocation.
  ///
  /// One tool-call event is one delivery, whether it invokes only its handler or both
  /// its handler and ToolCallCallback observer.
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
/// The string view is borrowed only for the callback invocation and may aggregate more
/// than one provider token. Copy it before returning if it must be retained.
/// Exceptions propagate synchronously out of Harness::update() after delivery counts.
using TextDeltaCallback = UniqueFunction<void(std::string_view)>;

/// Callback observing an accepted tool call after a model-visible result is queued.
///
/// The referenced ToolCall is borrowed for the invocation. Dispatch has already
/// produced either a successful result or a tool-error result when this observer fires;
/// exceptions propagate synchronously from update().
using ToolCallCallback = UniqueFunction<void(const ToolCall&)>;

/// Callbacks delivered on the Harness::update() thread for one turn.
///
/// Every member is optional; an empty member simply means no delivery of that kind.
/// Callbacks are moved into Harness-owned pump state by Harness::send() and are
/// attached atomically before the accepted command becomes visible to the worker.
/// Every nonempty member runs only on the thread calling Harness::update().
///
/// Callback exceptions are application exceptions, not Scry errors: they propagate
/// synchronously from update() after the event is counted as delivered. Callbacks may
/// call send(), Turn::cancel(), or ToolRegistry::add() reentrantly; a reentrant
/// update() call is rejected without delivering events.
struct TurnCallbacks {
  /// Observes coalesced fragments of streamed assistant text.
  TextDeltaCallback on_text_delta{};
  /// Observes accepted tool calls after their model-visible results are queued.
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
