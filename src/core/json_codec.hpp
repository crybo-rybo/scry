#pragma once

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

// The library's single Error factory. It lives in the bottom layer so the
// codec, the provider adapters, and the transport all build errors the same
// way instead of each keeping a private copy.
[[nodiscard]] Error make_error(ErrorCategory category, std::string message,
                               bool retryable = false);

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
