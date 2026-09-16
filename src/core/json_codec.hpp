#pragma once

#include "core/error.hpp"

#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace scry::detail {

// The one JSON document type in the library. The sorted map keeps every
// re-serialized document in a single canonical key order, so equal documents
// always produce equal bytes no matter which layer parsed them.
using JsonValue = glz::generic_sorted_u64;

// Stored JSON text spliced into a larger document as a nested value rather than
// re-parsed into a JsonValue first. A member of this type writes its bytes
// verbatim; a std::string_view member next to it writes the same bytes as a
// quoted, escaped JSON string.
using JsonText = glz::raw_json_view;

// Encodes a typed wire aggregate straight to JSON text. Glaze reflects a plain
// aggregate member by member in declaration order, so a wire struct whose
// members are declared alphabetically leaves the encoder in the same canonical
// key order a JsonValue would have produced - without building the tree.
template <class Wire>
[[nodiscard]] Result<std::string>
write_wire_json(const Wire& wire, const ErrorCategory category,
                const std::string_view failure_message) {
  std::string text{};
  if (glz::write_json(wire, text)) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return text;
}

// Reads into an existing value rather than returning one, so a caller that
// already owns storage for the document never moves or copies a JsonValue.
[[nodiscard]] Status parse_json_into(JsonValue& destination, std::string_view input,
                                     ErrorCategory category,
                                     std::string_view failure_message);

[[nodiscard]] Result<JsonValue> parse_json(std::string_view input,
                                           ErrorCategory category,
                                           std::string_view failure_message);

[[nodiscard]] Result<std::string> write_json_text(const JsonValue& value,
                                                  ErrorCategory category,
                                                  std::string_view failure_message);

[[nodiscard]] Result<Json> write_json(const JsonValue& value, ErrorCategory category,
                                      std::string_view failure_message);

[[nodiscard]] Result<Json> canonicalize_json(const Json& json, ErrorCategory category,
                                             std::string_view failure_message);

[[nodiscard]] Result<Json> canonicalize_json_object(const Json& json,
                                                    ErrorCategory category,
                                                    std::string_view failure_message);

[[nodiscard]] Json make_json_error_object(std::string_view message);

[[nodiscard]] const JsonValue* json_field(const JsonValue& value,
                                          std::string_view name) noexcept;

[[nodiscard]] Result<std::string_view> required_json_string(const JsonValue& value,
                                                            std::string_view name);

[[nodiscard]] Result<const JsonValue::array_t*>
required_json_array(const JsonValue& value, std::string_view name);

[[nodiscard]] Result<const JsonValue*> required_json_object(const JsonValue& value,
                                                            std::string_view name);

[[nodiscard]] Result<std::optional<std::string_view>>
optional_json_string(const JsonValue& value, std::string_view name);

[[nodiscard]] Result<std::optional<std::uint64_t>>
optional_json_uint(const JsonValue& value, std::string_view name);

} // namespace scry::detail
