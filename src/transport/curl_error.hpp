/// @file
/// @brief Translation from curl/HTTP retry metadata to Scry error values.
///
/// Curl codes never escape the transport implementation. These helpers give
/// callback failures and explicit cancellation precedence, classify remaining
/// curl failures, and decode bounded `Retry-After` metadata.

#pragma once

#include "core/transport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <scry/error.hpp>

namespace scry::detail::curl_error {

/// @brief Cooperative signal responsible for aborting a curl transfer.
enum class AbortCause : std::uint8_t {
  none,             ///< Curl ended without either Scry cancellation signal.
  turn_cancelled,   ///< The active turn's atomic cancellation flag was set.
  harness_shutdown, ///< The Harness worker stop token requested shutdown.
};

/// @brief Builds the cancellation error appropriate to an abort source.
/// @param cause Turn cancellation or Harness shutdown.
/// @return Non-retryable `cancelled` error with scope-appropriate wording.
[[nodiscard]] Error cancelled(AbortCause cause);

/// @brief Classifies the final failure of a curl transfer.
///
/// A specific error returned by a C-callback trampoline wins, followed by an
/// observed cancellation cause. Remaining codes map TLS/invalid response/
/// resource failures specially and ordinary transfer failures to retryable
/// `network` errors.
///
/// @param code Integer representation of the private `CURLcode`.
/// @param callback_error Error captured rather than unwound through the C ABI.
/// @param abort_cause Cancellation signal observed by the progress callback.
/// @return One sanitized Scry error.
[[nodiscard]] Error classify(int code, const std::optional<Error>& callback_error,
                             AbortCause abort_cause);

/// @brief Extracts the first usable HTTP `Retry-After` header.
/// @param headers Final response headers.
/// @return Nonnegative delay for delta-seconds or a future HTTP date, saturated
///         when necessary; `nullopt` if no header is parseable.
[[nodiscard]] std::optional<std::chrono::milliseconds>
retry_after(const std::vector<HttpHeader>& headers);

} // namespace scry::detail::curl_error
