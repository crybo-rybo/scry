#pragma once

#include <scry/error.hpp>
#include <scry/json.hpp>

namespace scry::reflection::detail {

[[nodiscard]] Result<Json> canonicalize_encoded_json(const Json& json);

} // namespace scry::reflection::detail
