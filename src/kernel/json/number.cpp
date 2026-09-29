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
// Decimal to binary64.
//
// Most JSON numbers take the exact path (Clinger's fast path): at most 19
// significant digits whose integer value is exactly a double, scaled by an
// exactly representable power of ten, which one correctly rounded IEEE
// multiplication or division finishes. Everything else goes through Decimal, a
// port of the arbitrary-precision decimal conversion in Go's strconv (the
// "simple decimal conversion" also behind fast_float's slow path): it halves or
// doubles an 800-digit decimal until the binary exponent is known, then rounds
// the 53 significant bits out of it, ties to even, remembering whether any
// nonzero digit fell off the end so an apparent tie is broken correctly.
//
// The kernel cannot use std::from_chars for doubles: the libc++ the Clang
// tooling build uses does not have it, and the fuzzers must exercise the code
// every build runs.
// ---------------------------------------------------------------------------

constexpr std::size_t max_fast_digits = 19;
constexpr std::uint64_t max_exact_integer = std::uint64_t{1} << 53U;
constexpr std::int64_t max_exact_power = 22;
constexpr std::array<double, 23> exact_powers_of_ten = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

struct Significand {
  // The first max_fast_digits significant digits as an integer.
  std::uint64_t value{};
  // Every digit after the leading zeros, trailing zeros included.
  std::size_t digits{};
};

void accumulate_significand(Significand& significand, const std::string_view digits) {
  for (const char digit : digits) {
    if (significand.digits == 0 && digit == '0') {
      continue;
    }
    if (significand.digits < max_fast_digits) {
      significand.value =
          significand.value * 10U + static_cast<std::uint64_t>(digit - '0');
    }
    ++significand.digits;
  }
}

[[nodiscard]] double with_sign(const bool negative, const double magnitude) noexcept {
  return negative ? -magnitude : magnitude;
}

[[nodiscard]] std::optional<double>
exact_path(const DecimalNumber& number, const Significand& significand) noexcept {
  if (significand.digits > max_fast_digits) {
    return std::nullopt;
  }
  if (significand.value == 0) {
    return with_sign(number.negative, 0.0);
  }
  const auto power =
      number.exponent - static_cast<std::int64_t>(number.fraction.size());
  if (significand.value > max_exact_integer || power < -max_exact_power ||
      power > max_exact_power) {
    return std::nullopt;
  }
  const auto integer = static_cast<double>(significand.value);
  const auto scale =
      exact_powers_of_ten[static_cast<std::size_t>(power < 0 ? -power : power)];
  return with_sign(number.negative, power < 0 ? integer / scale : integer * scale);
}

// binary64 layout, as Go's strconv names its parts.
constexpr int mantissa_bits = 52;
constexpr int exponent_bits = 11;
constexpr int exponent_bias = -1023;
constexpr int max_biased_exponent = (1 << exponent_bits) - 1;

class Decimal {
public:
  explicit Decimal(const DecimalNumber& number) noexcept;

  // The binary64 magnitude, or empty on overflow or on underflow of a nonzero
  // value to zero.
  [[nodiscard]] std::optional<std::uint64_t> to_bits() noexcept;

private:
  static constexpr std::size_t capacity = 800;
  // The widest shift whose carry still fits a uint64: 10 * 2^60 < 2^64.
  static constexpr unsigned max_shift = 60;

  void append(char digit) noexcept;
  void shift(std::int64_t bits) noexcept;
  void left_shift(unsigned bits) noexcept;
  void right_shift(unsigned bits) noexcept;
  void trim() noexcept;
  [[nodiscard]] std::uint64_t rounded_integer() const noexcept;
  [[nodiscard]] bool should_round_up(std::int64_t position) const noexcept;
  [[nodiscard]] int scale_to_unit_interval() noexcept;

  // Most significant first, as values 0-9; the value is 0.digits * 10^point.
  std::array<std::uint8_t, capacity> digits_{};
  std::size_t count_{};
  std::int64_t point_{};
  // Some nonzero digit was dropped for lack of room.
  bool truncated_{};
};

Decimal::Decimal(const DecimalNumber& number) noexcept {
  for (const char digit : number.integer) {
    if (count_ == 0 && digit == '0') {
      continue;
    }
    append(digit);
    ++point_;
  }
  for (const char digit : number.fraction) {
    if (count_ == 0 && digit == '0') {
      --point_;
      continue;
    }
    append(digit);
  }
  point_ += number.exponent;
  trim();
}

void Decimal::append(const char digit) noexcept {
  if (count_ < capacity) {
    digits_[count_++] = static_cast<std::uint8_t>(digit - '0');
  } else if (digit != '0') {
    truncated_ = true;
  }
}

void Decimal::trim() noexcept {
  while (count_ > 0 && digits_[count_ - 1] == 0) {
    --count_;
  }
  if (count_ == 0) {
    point_ = 0;
  }
}

void Decimal::shift(std::int64_t bits) noexcept {
  if (count_ == 0) {
    return;
  }
  for (; bits > max_shift; bits -= max_shift) {
    left_shift(max_shift);
  }
  for (; bits < -static_cast<std::int64_t>(max_shift); bits += max_shift) {
    right_shift(max_shift);
  }
  if (bits > 0) {
    left_shift(static_cast<unsigned>(bits));
  } else if (bits < 0) {
    right_shift(static_cast<unsigned>(-bits));
  }
}

void Decimal::left_shift(const unsigned bits) noexcept {
  // Least significant digit first; a carry below 10 * 2^bits adds at most 20
  // digits in front.
  std::array<std::uint8_t, capacity + 20> reversed{};
  std::size_t produced = 0;
  std::uint64_t carry = 0;
  for (auto read = count_; read-- > 0;) {
    carry += std::uint64_t{digits_[read]} << bits;
    const auto quotient = carry / 10U;
    reversed[produced++] = static_cast<std::uint8_t>(carry - quotient * 10U);
    carry = quotient;
  }
  for (; carry > 0; carry /= 10U) {
    reversed[produced++] = static_cast<std::uint8_t>(carry % 10U);
  }
  point_ += static_cast<std::int64_t>(produced - count_);
  count_ = std::min(produced, capacity);
  for (std::size_t index = 0; index < produced; ++index) {
    const auto digit = reversed[produced - 1 - index];
    if (index < count_) {
      digits_[index] = digit;
    } else if (digit != 0) {
      truncated_ = true;
    }
  }
  trim();
}

void Decimal::right_shift(const unsigned bits) noexcept {
  std::size_t read = 0;
  std::size_t write = 0;
  std::uint64_t value = 0;
  // Gather enough leading digits for the first quotient digit to be nonzero.
  while ((value >> bits) == 0) {
    if (read < count_) {
      value = value * 10U + digits_[read];
    } else {
      value *= 10U;
    }
    ++read;
  }
  point_ -= static_cast<std::int64_t>(read) - 1;
  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1U;
  for (; read < count_; ++read) {
    digits_[write++] = static_cast<std::uint8_t>(value >> bits);
    value = (value & mask) * 10U + digits_[read];
  }
  for (; value > 0; value = (value & mask) * 10U) {
    const auto digit = static_cast<std::uint8_t>(value >> bits);
    if (write < capacity) {
      digits_[write++] = digit;
    } else if (digit != 0) {
      truncated_ = true;
    }
  }
  count_ = write;
  trim();
}

bool Decimal::should_round_up(const std::int64_t position) const noexcept {
  if (position < 0 || static_cast<std::size_t>(position) >= count_) {
    return false;
  }
  const auto index = static_cast<std::size_t>(position);
  if (digits_[index] == 5 && index + 1 == count_) {
    // Exactly halfway unless something was dropped: ties to even.
    return truncated_ || (index > 0 && digits_[index - 1] % 2U == 1U);
  }
  return digits_[index] >= 5;
}

std::uint64_t Decimal::rounded_integer() const noexcept {
  std::uint64_t value = 0;
  std::int64_t index = 0;
  for (; index < point_ && static_cast<std::size_t>(index) < count_; ++index) {
    value = value * 10U + digits_[static_cast<std::size_t>(index)];
  }
  for (; index < point_; ++index) {
    value *= 10U;
  }
  return should_round_up(point_) ? value + 1U : value;
}

int Decimal::scale_to_unit_interval() noexcept {
  // Bits per step by decimal point position, so no step overshoots [0.5, 1).
  constexpr std::array<int, 9> step_bits = {1, 3, 6, 9, 13, 16, 19, 23, 26};
  constexpr int max_step_bits = 27;
  const auto step = [&](const std::int64_t point) {
    return point < static_cast<std::int64_t>(step_bits.size())
               ? step_bits[static_cast<std::size_t>(point)]
               : max_step_bits;
  };
  int exponent = 0;
  while (point_ > 0) {
    const auto bits = step(point_);
    shift(-bits);
    exponent += bits;
  }
  while (point_ < 0 || (point_ == 0 && digits_[0] < 5)) {
    const auto bits = step(-point_);
    shift(bits);
    exponent -= bits;
  }
  return exponent;
}

std::optional<std::uint64_t> Decimal::to_bits() noexcept {
  // The caller handles a zero mantissa; these bounds are Go's for binary64.
  if (point_ > 310 || point_ < -330) {
    return std::nullopt;
  }
  // Now in [0.5, 1); one less makes it [1, 2).
  auto exponent = scale_to_unit_interval() - 1;
  if (exponent < exponent_bias + 1) {
    const auto denormal_shift = exponent_bias + 1 - exponent;
    shift(-denormal_shift);
    exponent += denormal_shift;
  }
  if (exponent - exponent_bias >= max_biased_exponent) {
    return std::nullopt;
  }
  shift(1 + mantissa_bits);
  auto mantissa = rounded_integer();
  if (mantissa == std::uint64_t{2} << mantissa_bits) {
    mantissa >>= 1U;
    ++exponent;
    if (exponent - exponent_bias >= max_biased_exponent) {
      return std::nullopt;
    }
  }
  if (mantissa == 0) {
    return std::nullopt;
  }
  if ((mantissa & (std::uint64_t{1} << mantissa_bits)) == 0) {
    exponent = exponent_bias;
  }
  const auto biased = static_cast<std::uint64_t>(exponent - exponent_bias);
  return (mantissa & ((std::uint64_t{1} << mantissa_bits) - 1U)) |
         (biased << mantissa_bits);
}

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

std::optional<double> decimal_to_double(const DecimalNumber& number) noexcept {
  Significand significand{};
  accumulate_significand(significand, number.integer);
  accumulate_significand(significand, number.fraction);
  if (const auto exact = exact_path(number, significand)) {
    return exact;
  }
  Decimal decimal{number};
  const auto bits = decimal.to_bits();
  if (!bits) {
    return std::nullopt;
  }
  return with_sign(number.negative, std::bit_cast<double>(*bits));
}

bool decimal_in_range(const DecimalNumber& number) noexcept {
  // The decimal exponent of the first significant digit, so the value lies in
  // [10^leading, 10^(leading + 1)). Anywhere in [1e-307, 1e308) is a finite,
  // normal, nonzero double whatever the digits, so only numbers near either
  // limit need converting.
  constexpr std::int64_t safe_exponent = 307;
  const auto first_integer = number.integer.find_first_not_of('0');
  std::int64_t leading = 0;
  if (first_integer != std::string_view::npos) {
    leading = static_cast<std::int64_t>(number.integer.size() - first_integer) - 1;
  } else {
    const auto first_fraction = number.fraction.find_first_not_of('0');
    if (first_fraction == std::string_view::npos) {
      return true;
    }
    leading = -static_cast<std::int64_t>(first_fraction) - 1;
  }
  leading += number.exponent;
  if (leading >= -safe_exponent && leading <= safe_exponent) {
    return true;
  }
  return decimal_to_double(number).has_value();
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
