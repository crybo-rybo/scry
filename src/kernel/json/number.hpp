#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scry::detail::json {

// A JSON number token the scanner has already checked against the RFC 8259
// grammar, split into its parts. `integer` and `fraction` hold only digits;
// `fraction` is empty when the token has no fraction. `exponent` is the
// token's decimal exponent, saturated at +/-max_exponent, which no
// in-memory document can offset with enough mantissa digits to matter.
struct DecimalNumber {
  static constexpr std::int64_t max_exponent = 1'000'000'000'000'000;

  bool negative{};
  std::string_view integer{};
  std::string_view fraction{};
  std::int64_t exponent{};
};

// Converts a decimal number to the nearest binary64 value, ties to even, for any
// number of digits. Returns empty when the magnitude rounds to infinity, or when
// a nonzero value rounds to zero, so a number that JSON can spell but a double
// cannot hold is rejected rather than silently changed to infinity or zero. A
// zero mantissa is zero whatever its exponent, keeping the sign.
[[nodiscard]] std::optional<double>
decimal_to_double(const DecimalNumber& number) noexcept;

// True exactly when decimal_to_double(number) has a value, without converting
// unless the number is within a few decimal orders of the binary64 limits.
[[nodiscard]] bool decimal_in_range(const DecimalNumber& number) noexcept;

// Appends the canonical spelling of a finite double: the shortest decimal that
// reads back as the same double. A decimal exponent from -4 through 15 is
// spelled positionally with no trailing ".0" (0.0001, 1, 1234567890123456.8);
// anything else in scientific form with an uppercase E, no '+', and no leading
// exponent zeros (1E-5, 1E16, 1.5E300). Zero is "0" and negative zero "-0".
void append_double(std::string& output, double value);

} // namespace scry::detail::json
