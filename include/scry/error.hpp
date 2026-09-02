#pragma once

/**
 * @file error.hpp
 * @brief The single error model and expected-based result aliases used by Scry.
 *
 * Scry reports semantic and operational failures as values. Calls rejected before
 * acceptance return Result or Status directly; failures after acceptance travel as an
 * Error in TurnCallbacks::on_finished. Allocation failure and exceptions thrown by
 * application callbacks are outside this error-as-value contract.
 */

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <scry/turn_id.hpp>
#include <string>

namespace scry {

/// Stable programmatic categories for all Scry-originated failures.
///
/// Categories describe the layer and recovery class of a failure. Applications should
/// branch on this enum, not parse Error::message or Error::provider_detail.
enum class ErrorCategory : std::uint8_t {
  /// Configuration or serialized state failed validation.
  invalid_config,
  /// An operation is invalid for the object's current state.
  invalid_state,
  /// A Conversation already has a queued or active turn.
  busy,
  /// Provider authentication failed.
  authentication,
  /// The provider rejected the request because of a rate limit.
  rate_limit,
  /// A network or transport operation failed.
  network,
  /// Provider input/output violated the selected wire protocol or JSON contract.
  protocol,
  /// A configured admission, payload, or memory bound was exceeded.
  resource_limit,
  /// Tool dispatch, arguments, handler execution, or results failed.
  tool,
  /// A turn requested more tool rounds than Config::max_tool_rounds permits.
  max_tool_rounds,
  /// The turn was cancelled cooperatively.
  cancelled,
};

/// Error value used across every public fallible operation.
///
/// Provider-supplied fields are sanitized before they reach this boundary. API keys,
/// auth headers, prompt content, and tool content are never intentionally included.
/// Fields that do not apply retain their default empty/zero value, keeping one error
/// representation usable before admission, during provider I/O, and at terminal turn
/// delivery.
///
/// @note retryable describes whether a later application attempt may succeed. It does
/// not promise that Scry will retry this occurrence; automatic retry is restricted to
/// failures before semantic output and is bounded by RetryPolicy.
struct Error {
  // Compact scalar header. These fields intentionally remain adjacent because Error
  // travels by value through expected objects and event queues.
  /// Stable programmatic category.
  ErrorCategory category{ErrorCategory::invalid_state};
  /// Whether retrying may succeed. Scry retries automatically only before semantic
  /// output.
  bool retryable{false};
  /// One-based request attempt number, or zero when no request was attempted.
  std::uint32_t attempt{};
  /// Human-readable Scry diagnostic.
  ///
  /// This text is intended for logs or UI diagnostics and is not a stable identifier.
  std::string message{};
  /// Sanitized provider diagnostic, when one is safe and available.
  std::string provider_detail{};
  /// Provider-requested retry delay parsed from a valid `Retry-After`, when present.
  std::optional<std::chrono::milliseconds> retry_after{};
  /// Correlated turn, when the failure belongs to an accepted turn.
  std::optional<TurnId> turn_id{};
  /// Sanitized opaque provider request identifier, when available.
  std::string provider_request_id{};
};

/// Result of a fallible Scry operation that produces a value.
/// @tparam T Success value type.
template <typename T> using Result = std::expected<T, Error>;

/// Result of a fallible Scry operation whose success carries no additional value.
using Status = Result<void>;

} // namespace scry
