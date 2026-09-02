/// @file
/// @brief Small safety helpers shared by both provider dialect adapters.
///
/// Only behavior that is intentionally identical between dialects belongs
/// here. Their stream lifecycle machines and wire mappings remain independent.

#pragma once

#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "core/provider.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace scry::detail {

/// @brief Reduces an untrusted provider error identifier to a safe token.
///
/// Provider error identifiers can reach `Error::provider_detail`, so only a
/// nonempty, at-most-96-byte alphanumeric/underscore token survives. Everything
/// else collapses to the fixed `unknown_error` placeholder.
///
/// @param value Untrusted provider-controlled bytes.
/// @return Owning sanitized token safe for diagnostics.
[[nodiscard]] inline std::string sanitize_error_token(const std::string_view value) {
  constexpr std::size_t maximum_bytes = 96;
  if (value.empty() || value.size() > maximum_bytes) {
    return "unknown_error";
  }
  const auto safe = std::ranges::all_of(value, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '_';
  });
  return safe ? std::string{value} : std::string{"unknown_error"};
}

/// @brief Claims or retrieves the dialect alternative in shared decode state.
/// @tparam Dialect One alternative of `ProviderDialectDecodeState`.
/// @param state Per-request state to claim.
/// @param message Sanitized protocol diagnostic used on dialect mismatch.
/// @return Borrowed mutable dialect state, or a protocol error when another
///         dialect already owns the context.
template <typename Dialect>
[[nodiscard]] Result<Dialect*> dialect_decode_state(ProviderDecodeState& state,
                                                    const std::string_view message) {
  if (std::holds_alternative<std::monostate>(state.dialect)) {
    state.dialect.template emplace<Dialect>();
  }
  auto* decode = std::get_if<Dialect>(&state.dialect);
  if (decode == nullptr) {
    return std::unexpected(make_error(ErrorCategory::protocol, std::string{message}));
  }
  return decode;
}

/// @brief Rejects wire input after the decoder emitted its terminal event.
///
/// Completion moves the accumulated response out of `state`; this guard must
/// run before later handlers inspect that moved-from value.
///
/// @param state Per-request decode state.
/// @param message Sanitized dialect-specific protocol diagnostic.
/// @return Success before terminal state, otherwise a protocol error.
[[nodiscard]] inline Status
reject_event_after_terminal(const ProviderDecodeState& state,
                            const std::string_view message) {
  if (state.completed) {
    return std::unexpected(make_error(ErrorCategory::protocol, std::string{message}));
  }
  return {};
}

/// @brief Checks an argument fragment against its per-call byte ceiling.
///
/// Validation occurs before append, so retained untrusted input can never grow
/// beyond the configured limit. The subtraction form is overflow-safe.
///
/// @param accumulated Bytes already retained for the call.
/// @param fragment Bytes in the prospective fragment.
/// @param limit Configured maximum bytes for one call's arguments.
/// @param message Sanitized diagnostic for overflow/limit failure.
/// @return Success when the append fits, otherwise `resource_limit`.
[[nodiscard]] inline Status accept_tool_argument_bytes(const std::size_t accumulated,
                                                       const std::size_t fragment,
                                                       const std::size_t limit,
                                                       const std::string_view message) {
  if (accumulated > limit || fragment > limit - accumulated) {
    return std::unexpected(
        make_error(ErrorCategory::resource_limit, std::string{message}));
  }
  return {};
}

} // namespace scry::detail
