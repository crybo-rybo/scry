#include "kernel/kernel.hpp"

#include "kernel/json/number.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace scry::detail::json {
namespace {

// ---------------------------------------------------------------------------
// binary64 to canonical text.
// ---------------------------------------------------------------------------

// The canonical form's positional range, by decimal exponent of the first
// significant digit; outside it the number is written in scientific form.
constexpr int min_positional_exponent = -4;
constexpr int max_positional_exponent = 15;

// A double's spelling, assembled in place and appended once. The longest is a
// sign, "0.000", and 17 digits, or a sign, 17 digits, a point, and "E-324".
class Spelling {
public:
  void push(const char character) noexcept { text_[size_++] = character; }

  void push(const std::string_view characters) noexcept {
    for (const char character : characters) {
      push(character);
    }
  }

  void zeros(std::size_t count) noexcept {
    for (; count > 0; --count) {
      push('0');
    }
  }

  void append_to(std::string& output) const { output.append(text_.data(), size_); }

private:
  std::array<char, 32> text_{};
  std::size_t size_{};
};

// The shortest round-trip decimal: its significant digits, without a point or
// trailing zeros, and the decimal exponent of the first one.
struct ShortestDecimal {
  std::array<char, 17> digit_storage{};
  std::size_t count{};
  int exponent{};

  [[nodiscard]] std::string_view digits() const noexcept {
    return {digit_storage.data(), count};
  }
};

// Reads std::to_chars' scientific spelling of a positive finite double,
// "d[.ddd]e(+|-)XX", which is the shortest that round-trips.
[[nodiscard]] ShortestDecimal shortest_decimal(const double magnitude) noexcept {
  std::array<char, 32> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                     magnitude, std::chars_format::scientific);
  ShortestDecimal decimal{};
  const char* cursor = buffer.data();
  for (; *cursor != 'e'; ++cursor) {
    if (*cursor != '.') {
      decimal.digit_storage[decimal.count++] = *cursor;
    }
  }
  const bool negative = cursor[1] == '-';
  for (cursor += 2; cursor != written.ptr; ++cursor) {
    decimal.exponent = decimal.exponent * 10 + (*cursor - '0');
  }
  if (negative) {
    decimal.exponent = -decimal.exponent;
  }
  return decimal;
}

void spell_positional(Spelling& spelling, const ShortestDecimal& decimal) noexcept {
  const auto digits = decimal.digits();
  if (decimal.exponent < 0) {
    spelling.push("0.");
    spelling.zeros(static_cast<std::size_t>(-decimal.exponent - 1));
    spelling.push(digits);
    return;
  }
  const auto whole = static_cast<std::size_t>(decimal.exponent) + 1U;
  if (digits.size() <= whole) {
    spelling.push(digits);
    spelling.zeros(whole - digits.size());
    return;
  }
  spelling.push(digits.substr(0, whole));
  spelling.push('.');
  spelling.push(digits.substr(whole));
}

void spell_scientific(Spelling& spelling, const ShortestDecimal& decimal) noexcept {
  const auto digits = decimal.digits();
  spelling.push(digits.front());
  if (digits.size() > 1) {
    spelling.push('.');
    spelling.push(digits.substr(1));
  }
  spelling.push('E');
  if (decimal.exponent < 0) {
    spelling.push('-');
  }
  const auto magnitude = decimal.exponent < 0 ? -decimal.exponent : decimal.exponent;
  std::array<char, 4> buffer{};
  const auto written =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), magnitude);
  spelling.push(std::string_view{buffer.data(), written.ptr});
}

} // namespace

std::optional<double> parse_double(const char* first, const char* last) noexcept {
  double value{};
  const auto [ptr, error] =
      std::from_chars(first, last, value, std::chars_format::general);
  if (error != std::errc{} || ptr != last) {
    return std::nullopt;
  }
  return value;
}

void append_double(std::string& output, const double value) {
  // A whole double below 2^53 is an exact integer whose shortest spelling is
  // its own digits (a neighbour is at most 1 away, so no shorter decimal rounds
  // to it), and below 10^16 the canonical form spells it positionally.
  constexpr double max_exact_whole = 9007199254740992.0;
  if (value != 0.0 && value > -max_exact_whole && value < max_exact_whole) {
    const auto integer = static_cast<std::int64_t>(value);
    if (static_cast<double>(integer) == value) {
      std::array<char, 24> whole{};
      const auto end =
          std::to_chars(whole.data(), whole.data() + whole.size(), integer);
      output.append(whole.data(), end.ptr);
      return;
    }
  }
  Spelling spelling{};
  if (std::signbit(value)) {
    spelling.push('-');
  }
  if (value == 0.0) {
    spelling.push('0');
  } else {
    const auto decimal = shortest_decimal(std::fabs(value));
    if (decimal.exponent >= min_positional_exponent &&
        decimal.exponent <= max_positional_exponent) {
      spell_positional(spelling, decimal);
    } else {
      spell_scientific(spelling, decimal);
    }
  }
  spelling.append_to(output);
}

} // namespace scry::detail::json
