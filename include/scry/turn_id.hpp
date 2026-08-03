#pragma once

#include <compare>
#include <cstdint>

namespace scry {

/// Immutable correlation identifier assigned to an accepted turn.
struct TurnId {
  /// Numeric identifier. Zero is the invalid or unset value.
  std::uint64_t value{};

  /// Reports whether this identifier names an accepted turn.
  /// @return true when value is nonzero.
  [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0; }

  /// Provides value ordering and equality.
  /// @return Three-way comparison result.
  auto operator<=>(const TurnId&) const = default;
};

} // namespace scry
