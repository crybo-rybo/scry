#pragma once

#include "kernel/error.hpp"

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

// Validates JSON text without materializing a document: one allocation-free
// skip pass over every byte that rejects malformed interiors, truncation,
// trailing garbage, and a second document. The object variant also requires the
// root to be an object. Surrounding whitespace is accepted, as any parser would.
[[nodiscard]] Status validate_json(std::string_view input, ErrorCategory category,
                                   std::string_view failure_message);

[[nodiscard]] Status validate_json_object(std::string_view input,
                                          ErrorCategory category,
                                          std::string_view failure_message);

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

// A double spelled as the canonical writer spells it. That is the shortest
// round-trip digits, as std::to_chars writes them, but laid out positionally for
// decimal exponents from -4 to 15 and otherwise as `1E-7`, where std::to_chars
// chooses by length and writes `1e-07`. A non-finite value is written as null.
[[nodiscard]] Json canonical_json_number(double value);

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
