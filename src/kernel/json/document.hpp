#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Scry's own strict JSON layer: an immutable document tree, a single-pass
// parser, an allocation-free validator that accepts exactly what the parser
// accepts, and the canonical writer.
//
// What the parser accepts is RFC 8259 JSON text holding exactly one value, with
// these decisions where the RFC leaves room:
//
// - Nesting: at most max_depth arrays and objects open at once, counting an
//   empty one. Deeper input is rejected, which bounds the parser's and the
//   tree's recursion.
// - Whitespace: only space, tab, line feed, and carriage return, around any
//   token. Input with no value at all, whitespace only included, is rejected,
//   as are a byte order mark, a NUL, and anything after the value.
// - Strings: UTF-8 is validated strictly, in keys and values: no overlong
//   forms, no encoded surrogates, nothing above U+10FFFF, no truncated
//   sequence. Control characters below 0x20 must be escaped. A \u escape of a
//   surrogate must be a high surrogate followed at once by a \u escape of a
//   low one; a lone or reversed surrogate is rejected. \u0000 is accepted and
//   decodes to a NUL byte.
// - Numbers: a number spelled as an optional '-' and digits alone is an
//   integer: unsigned_integer when not negative and within uint64,
//   signed_integer when negative and within int64. "-0" is not negative, and
//   is the number -0.0 so its sign survives a canonical round trip. Every
//   other number, including one with a fraction or an exponent even when its
//   value is whole (1.0, 1e2), and an integer outside those ranges, is a
//   double, correctly rounded. A number whose magnitude rounds to infinity,
//   or a nonzero one that rounds to zero, is rejected.
// - Duplicate object keys: the last occurrence wins, at every level.
//
// Canonical text has no insignificant whitespace, object keys in lexical byte
// order, strings with only the escapes JSON requires (\" \\ \b \f \n \r \t,
// and \u00XX with uppercase hex for any other control character; '/' and
// everything from 0x7F up are written as they are), and numbers as described
// by append_double in kernel/json/number.hpp.

namespace scry::detail::json {

// The deepest nesting the parser accepts.
inline constexpr std::size_t max_depth = 256;

class Value;

using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
// Sorted by key in lexical byte order, keys unique.
using Object = std::vector<Member>;

// One immutable JSON value. Only the parser builds one with content; a
// default-constructed value is null. Objects are sorted vectors, so lookup is
// a binary search and ordered iteration is the canonical order. Destruction
// recurses once per nesting level, which max_depth bounds.
class Value final {
public:
  Value() noexcept = default;

  [[nodiscard]] JsonKind kind() const noexcept;

  [[nodiscard]] bool is_null() const noexcept;

  [[nodiscard]] std::optional<bool> boolean() const noexcept;

  // Only for kind() == JsonKind::signed_integer.
  [[nodiscard]] std::optional<std::int64_t> signed_integer() const noexcept;

  // Only for kind() == JsonKind::unsigned_integer.
  [[nodiscard]] std::optional<std::uint64_t> unsigned_integer() const noexcept;

  // Any numeric kind, as a double; an integer converts to the nearest double.
  [[nodiscard]] std::optional<double> number() const noexcept;

  // Borrowed from this value.
  [[nodiscard]] std::optional<std::string_view> string() const noexcept;

  [[nodiscard]] const Array* array() const noexcept;

  [[nodiscard]] const Object* object() const noexcept;

  // The element count of an array or member count of an object; zero otherwise.
  [[nodiscard]] std::size_t size() const noexcept;

  // An object member's value, found by binary search; null when this is not an
  // object or has no such key.
  [[nodiscard]] const Value* find(std::string_view key) const noexcept;

private:
  friend class Builder;

  // Alternatives in JsonKind order, so kind() is the index.
  using Data = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double,
                            std::string, Array, Object>;

  explicit Value(Data data) noexcept : data_(std::move(data)) {}

  Data data_{};
};

// Parses exactly one JSON document, or returns empty for anything the rules
// above reject.
[[nodiscard]] std::optional<Value> parse(std::string_view text);

// True exactly when parse(text) would succeed, without allocating.
[[nodiscard]] bool validate(std::string_view text) noexcept;

// Appends the canonical text of `value`.
void write(const Value& value, std::string& output);

[[nodiscard]] std::string write(const Value& value);

// Appends `text` as a canonical JSON string literal, quotes included. Bytes
// from 0x7F up pass through unchanged, so valid UTF-8 in is valid UTF-8 out.
void append_quoted(std::string& output, std::string_view text);

[[nodiscard]] std::string quote(std::string_view text);

} // namespace scry::detail::json
