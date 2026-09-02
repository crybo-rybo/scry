#pragma once

/**
 * @file turn_id.hpp
 * @brief Small provider-independent identifier used to correlate turn activity.
 */

#include <compare>
#include <cstdint>

namespace scry {

/// Immutable correlation identifier assigned to an accepted turn.
///
/// Identifiers are monotonically allocated within one Harness and are intended for
/// correlation, logging, and equality checks. They are not process-global IDs and may
/// repeat across independent Harness instances. The aggregate remains constructible so
/// Error, ToolCall, and Completion stay ordinary value types.
struct TurnId {
  /// Numeric identifier. Zero is the invalid or unset value.
  std::uint64_t value{};

  /// Reports whether this identifier names an accepted turn.
  /// @return true when value is nonzero.
  [[nodiscard]] explicit constexpr operator bool() const noexcept { return value != 0; }

  /// Provides ordering and equality by the underlying numeric value.
  /// @param other Identifier to compare with this value.
  /// @return Three-way comparison result.
  auto operator<=>(const TurnId& other) const = default;
};

} // namespace scry
