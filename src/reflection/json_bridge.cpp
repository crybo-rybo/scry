#include "kernel/json/codec.hpp"

#include <scry/detail/reflection_json.hpp>

namespace scry::reflection::detail {

Result<Json> canonicalize_encoded_json(const Json& json) {
  return scry::detail::canonicalize_json(
      json, ErrorCategory::tool, "reflected value could not be encoded as JSON");
}

} // namespace scry::reflection::detail
