#pragma once

// Small helpers both dialect adapters need. The two stream state machines stay
// independent; only the pieces that are byte-for-byte identical live here.

#include "core/json_codec.hpp"
#include "core/provider.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace scry::detail {

// Provider error identifiers reach Error::provider_detail, so only a bounded
// alphanumeric token survives; anything else collapses to a fixed placeholder.
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

// Claims the shared decode state for one dialect, rejecting a state another
// dialect already owns.
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

// The terminal event moves the accumulated response out of the decode state,
// so every later event must be refused before anything reads it again.
[[nodiscard]] inline Status
reject_event_after_terminal(const ProviderDecodeState& state,
                            const std::string_view message) {
  if (state.completed) {
    return std::unexpected(make_error(ErrorCategory::protocol, std::string{message}));
  }
  return {};
}

// Rejects a tool-argument fragment before it is appended, so an over-limit
// stream never grows the destination past the configured budget.
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
