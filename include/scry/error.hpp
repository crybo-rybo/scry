#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <scry/turn_id.hpp>
#include <string>

namespace scry {

/// Stable categories for all Scry-originated failures.
enum class ErrorCategory : std::uint8_t {
  /// Configuration or serialized state failed validation.
  invalid_config,
  /// An operation is invalid for the object's current state.
  invalid_state,
  /// A caller-supplied argument failed validation, including a duplicate tool name.
  invalid_argument,
  /// A Conversation already has a queued or active turn.
  busy,
  /// Provider authentication failed.
  authentication,
  /// The provider rejected the request because of a rate limit.
  rate_limit,
  /// A network or transport operation failed.
  network,
  /// Provider output violated the selected wire protocol.
  protocol,
  /// A configured admission, payload, or memory bound was exceeded.
  resource_limit,
  /// Tool dispatch, arguments, or results were invalid.
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
struct Error {
  // Keep the scalar header together: Error is carried by value through expected
  // and event queues, so separating these fields adds padding to every instance.
  /// Stable programmatic category.
  ErrorCategory category{ErrorCategory::invalid_state};
  /// Whether retrying may succeed. Scry retries automatically only before semantic
  /// output.
  bool retryable{false};
  /// One-based request attempt number, or zero when no request was attempted.
  std::uint32_t attempt{};
  /// Human-readable Scry diagnostic.
  std::string message{};
  /// Sanitized provider diagnostic, when one is safe and available.
  std::string provider_detail{};
  /// Provider-requested retry delay, when supplied and valid.
  std::optional<std::chrono::milliseconds> retry_after{};
  /// Correlated turn, when the failure belongs to an accepted turn.
  std::optional<TurnId> turn_id{};
  /// Sanitized provider request identifier, when available.
  std::string provider_request_id{};
};

/// Result of a fallible Scry operation.
template <typename T> using Result = std::expected<T, Error>;

/// Result of a fallible operation that returns no value on success.
using Status = Result<void>;

} // namespace scry
