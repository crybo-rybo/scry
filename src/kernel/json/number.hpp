#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace scry::detail::json {

// Converts a JSON number token, [first, last), that the scanner has already checked
// against the RFC 8259 grammar to the nearest double, ties to even. Returns empty when
// the magnitude rounds to infinity, or when a nonzero value rounds to zero, so a number
// that JSON can spell but a double cannot hold is rejected rather than silently changed
// to infinity or zero. A zero mantissa is zero whatever its exponent, keeping the sign.
[[nodiscard]] std::optional<double> parse_double(const char* first,
                                                 const char* last) noexcept;

// Appends the canonical spelling of a finite double: the shortest decimal that
// reads back as the same double. A decimal exponent from -4 through 15 is
// spelled positionally with no trailing ".0" (0.0001, 1, 1234567890123456.8);
// anything else in scientific form with an uppercase E, no '+', and no leading
// exponent zeros (1E-5, 1E16, 1.5E300). Zero is "0" and negative zero "-0".
void append_double(std::string& output, double value);

} // namespace scry::detail::json
