#pragma once

#include <compare>
#include <cstdint>

namespace scry {

struct TurnId {
  std::uint64_t value{};

  [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0; }

  auto operator<=>(const TurnId&) const = default;
};

} // namespace scry
