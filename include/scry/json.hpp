#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace scry {

/// Scry-owned serialized JSON boundary value.
///
/// Callers provide syntactically valid JSON. Operations that require a specific shape
/// validate it and return ErrorCategory::invalid_config or ErrorCategory::tool as
/// appropriate. No third-party JSON type crosses Scry's public boundary. Use JsonView
/// to read one, and escape_json_string() to build one by hand.
struct Json {
  /// UTF-8 JSON text.
  std::string text{};
};

/// Kind of a JSON value observed through JsonView.
enum class JsonKind : std::uint8_t {
  /// A JSON null, and the kind reported by a default-constructed view.
  null,
  /// A JSON true or false.
  boolean,
  /// A whole number that fits a signed 64-bit integer.
  signed_integer,
  /// A whole number that fits an unsigned 64-bit integer.
  unsigned_integer,
  /// Any other numeric value.
  number,
  /// A JSON string.
  string,
  /// A JSON array.
  array,
  /// A JSON object.
  object,
};

/// Read-only view into a parsed JSON document.
///
/// Parsing produces one immutable document; every view into it shares ownership, so
/// a child view stays valid after the parent is destroyed. Object keys are visited in
/// lexicographic order, matching Scry's canonical form. A default-constructed view
/// reports JsonKind::null and every accessor returns empty.
class JsonView final {
public:
  /// Creates an empty view that reports JsonKind::null.
  JsonView() = default;

  /// Parses JSON text.
  /// @param json JSON text to parse.
  /// @return The root view, or ErrorCategory::invalid_argument when the text is not
  /// valid JSON.
  [[nodiscard]] static Result<JsonView> parse(const Json& json);

  /// Reports the kind of the viewed value.
  /// @return The observed JsonKind.
  [[nodiscard]] JsonKind kind() const noexcept;

  /// Reads a boolean value.
  /// @return The boolean, or empty when the value is not a boolean.
  [[nodiscard]] std::optional<bool> boolean() const noexcept;

  /// Reads a signed whole number.
  /// @return The value, or empty unless kind() is JsonKind::signed_integer.
  [[nodiscard]] std::optional<std::int64_t> signed_integer() const noexcept;

  /// Reads an unsigned whole number.
  /// @return The value, or empty unless kind() is JsonKind::unsigned_integer.
  [[nodiscard]] std::optional<std::uint64_t> unsigned_integer() const noexcept;

  /// Reads any numeric value as a double, including integers.
  /// @return The value, or empty when the value is not numeric.
  [[nodiscard]] std::optional<double> number() const noexcept;

  /// Reads a string value.
  /// @return The text, borrowed from the shared document, or empty when the value is
  /// not a string.
  [[nodiscard]] std::optional<std::string_view> string() const noexcept;

  /// Returns the element count of an array or object.
  /// @return The count, and zero for every other kind.
  [[nodiscard]] std::size_t size() const noexcept;

  /// Reads one array element.
  /// @param index Zero-based element index.
  /// @return The element view, or empty when the value is not an array or the index is
  /// out of range.
  [[nodiscard]] std::optional<JsonView> at(std::size_t index) const noexcept;

  /// Reads one object key in lexicographic order.
  /// @param index Zero-based key index.
  /// @return The key, borrowed from the shared document, or empty when the value is not
  /// an object or the index is out of range.
  [[nodiscard]] std::optional<std::string_view>
  key_at(std::size_t index) const noexcept;

  /// Looks up one object member by name.
  /// @param name Member name.
  /// @return The member view, or empty when the value is not an object or has no such
  /// member.
  [[nodiscard]] std::optional<JsonView> find(std::string_view name) const noexcept;

private:
  class Document;

  JsonView(std::shared_ptr<const Document> document, const void* value) noexcept;

  std::shared_ptr<const Document> document_{};
  const void* value_{};
};

/// Quotes and escapes one JSON string literal, for hosts that assemble small JSON
/// results by hand.
/// @param value Raw UTF-8 text. Non-ASCII bytes pass through unescaped.
/// @return The value as a quoted JSON string with every required escape applied.
[[nodiscard]] std::string escape_json_string(std::string_view value);

} // namespace scry
