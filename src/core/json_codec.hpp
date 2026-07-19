#pragma once

#include <glaze/glaze.hpp>
#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string_view>

namespace scry::detail {

using JsonValue = glz::generic_sorted_u64;

[[nodiscard]] Result<JsonValue> parse_json(std::string_view input,
                                           ErrorCategory category,
                                           std::string_view failure_message);

[[nodiscard]] Result<Json> write_json(const JsonValue& value, ErrorCategory category,
                                      std::string_view failure_message);

[[nodiscard]] Status require_json_object(const JsonValue& value, ErrorCategory category,
                                         std::string_view failure_message);

[[nodiscard]] Result<Json> canonicalize_json(const Json& json, ErrorCategory category,
                                             std::string_view failure_message);

[[nodiscard]] Result<Json> canonicalize_json_object(const Json& json,
                                                    ErrorCategory category,
                                                    std::string_view failure_message);

[[nodiscard]] Json make_json_error_object(std::string_view message);

} // namespace scry::detail
