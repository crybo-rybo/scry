#include "core/json_codec.hpp"

#include <scry/detail/reflection_json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry::reflection::detail {

Result<JsonView> parse_json(const Json& json) {
  auto view = JsonView::parse(json);
  if (!view) {
    return std::unexpected(Error{
        .category = ErrorCategory::tool,
        .message = "reflected tool arguments are not valid JSON",
    });
  }
  return std::move(*view);
}

void append_json_string(std::string& output, const std::string_view value) {
  output.append(escape_json_string(value));
}

Result<Json> canonicalize_encoded_json(const Json& json) {
  return scry::detail::canonicalize_json(
      json, ErrorCategory::tool, "reflected value could not be encoded as JSON");
}

} // namespace scry::reflection::detail
