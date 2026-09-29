#pragma once

#include "kernel/error.hpp"

#include <glaze/glaze.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

// The parsed JSON document, private to the kernel. JsonView wraps it for the
// rest of the library, and only kernel sources, which compile with Glaze's
// headers, include this.
namespace scry::detail {

// The one JSON document type in the library. The sorted map keeps every
// re-serialized document in a single canonical key order, so equal documents
// always produce equal bytes no matter which layer parsed them.
using JsonValue = glz::generic_sorted_u64;

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

[[nodiscard]] const JsonValue* json_field(const JsonValue& value,
                                          std::string_view name) noexcept;

} // namespace scry::detail
