#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace scry::detail {

[[nodiscard]] inline std::string
sanitize_anthropic_error_type(const std::string_view value) {
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

} // namespace scry::detail
