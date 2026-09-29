#include "kernel/json/codec.hpp"

#include <scry/detail/reflection_json.hpp>

namespace scry::reflection::detail {

Result<Json> canonicalize_encoded_json(const Json& json) {
  return scry::detail::canonicalize_json(
      json, ErrorCategory::tool, "reflected value could not be encoded as JSON");
}

bool is_valid_json_text(const std::string_view text) {
  return scry::detail::validate_json(text, ErrorCategory::tool,
                                     "reflected JSON payload is not valid JSON")
      .has_value();
}

} // namespace scry::reflection::detail
