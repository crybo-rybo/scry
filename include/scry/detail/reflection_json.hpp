#pragma once

/**
 * @file reflection_json.hpp
 * @brief Scry-owned immutable JSON view used by reflection templates.
 *
 * Public reflection templates must inspect parsed JSON without including or exposing
 * Scry's private JSON dependency. JsonView is the compiled bridge: one shared immutable
 * document owns the parsed tree while cheap child views retain that lifetime and point
 * at individual values.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace scry::reflection::detail {

/// Runtime JSON kinds distinguished by strict reflected decoding.
///
/// Signed integers, unsigned integers, and non-integral numbers stay distinct so an
/// integral C++ member cannot silently accept the lexical JSON number `1.0`.
enum class JsonKind : std::uint8_t {
  /// JSON null, also used by an empty/default JsonView.
  null,
  /// JSON boolean.
  boolean,
  /// Integer represented in the signed 64-bit domain.
  signed_integer,
  /// Nonnegative integer represented in the unsigned 64-bit domain.
  unsigned_integer,
  /// Non-integral JSON number represented as double by the bridge.
  number,
  /// JSON string.
  string,
  /// Ordered JSON array.
  array,
  /// JSON object with canonical unique-key lookup semantics.
  object,
};

/// Immutable, lifetime-owning view of one node in a parsed JSON document.
///
/// Copying a view shares ownership of its Document. Child views returned by at() and
/// find() therefore remain valid even after the parent view is destroyed. String and
/// key accessors return borrowed views into that shared immutable document and remain
/// valid only while at least one related JsonView keeps it alive.
///
/// A default-constructed view is empty: kind() reports JsonKind::null, scalar accessors
/// return `std::nullopt`, and size() returns zero. The type performs no mutation and is
/// used only inside the optional reflection component.
class JsonView final {
public:
  /// Constructs an empty view with no backing document.
  JsonView() = default;

  /// Reports the exact runtime category of the referenced node.
  /// @return Node kind, or JsonKind::null for an empty view.
  [[nodiscard]] JsonKind kind() const noexcept;
  /// Reads a JSON boolean without conversion.
  /// @return Stored value, or `std::nullopt` when the node is not boolean.
  [[nodiscard]] std::optional<bool> boolean() const noexcept;
  /// Reads a signed JSON integer without conversion.
  /// @return Stored value, or `std::nullopt` for every other numeric representation.
  [[nodiscard]] std::optional<std::int64_t> signed_integer() const noexcept;
  /// Reads an unsigned JSON integer without conversion.
  /// @return Stored value, or `std::nullopt` for every other numeric representation.
  [[nodiscard]] std::optional<std::uint64_t> unsigned_integer() const noexcept;
  /// Reads a JSON number through the bridge's double representation.
  /// @return Numeric value, or `std::nullopt` when the node is not numeric.
  [[nodiscard]] std::optional<double> number() const noexcept;
  /// Borrows the bytes of a JSON string.
  /// @return View into the backing document, or `std::nullopt` for a non-string node.
  [[nodiscard]] std::optional<std::string_view> string() const noexcept;

  /// Reports the number of children in an array or object.
  /// @return Array length, object member count, or zero for a scalar/empty view.
  [[nodiscard]] std::size_t size() const noexcept;
  /// Looks up an array element by zero-based position.
  /// @param index Element position.
  /// @return Child view, or `std::nullopt` when this is not an array or index is out of
  /// range.
  [[nodiscard]] std::optional<JsonView> at(std::size_t index) const noexcept;
  /// Borrows an object's key at its canonical iteration position.
  /// @param index Zero-based object-member position.
  /// @return Key view, or `std::nullopt` when this is not an object or index is out of
  /// range.
  [[nodiscard]] std::optional<std::string_view>
  key_at(std::size_t index) const noexcept;
  /// Looks up a named object member.
  /// @param name Exact UTF-8 key to find.
  /// @return Child view, or `std::nullopt` when this is not an object or the key is
  /// absent.
  [[nodiscard]] std::optional<JsonView> find(std::string_view name) const noexcept;

private:
  /// Shared owner of the private parsed tree referenced by this family of views.
  class Document;

  /// Constructs a view of one node owned by a parsed document.
  /// @param document Shared immutable owner that keeps value alive.
  /// @param value Non-owning pointer to a node inside document.
  JsonView(std::shared_ptr<const Document> document, const void* value) noexcept;

  /// Shared lifetime anchor for value_ and all borrowed strings returned by this view.
  std::shared_ptr<const Document> document_{};
  /// Non-owning pointer to a node inside document_, erased from the private JSON type.
  const void* value_{};

  /// Allows the bridge parser to create the root view over a new Document.
  friend Result<JsonView> parse_json(Json);
};

/// Parses serialized reflected input into an immutable lifetime-owning view tree.
///
/// The compiled definition documents the transferred input and error result; keeping
/// this declaration in the public template boundary prevents the private JSON type from
/// leaking into reflection instantiations.
[[nodiscard]] Result<JsonView> parse_json(Json json);
/// Appends one string using canonical JSON quoting and control-byte escaping.
///
/// Existing output bytes are retained so recursive template encoding can compose a
/// document without intermediate parsed values.
void append_json_string(std::string& output, std::string_view value);
/// Validates and canonicalizes text assembled by the reflected value encoder.
///
/// Invalid generated text is mapped to ErrorCategory::tool by the compiled bridge.
[[nodiscard]] Result<Json> canonicalize_encoded_json(Json json);

} // namespace scry::reflection::detail
