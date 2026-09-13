#pragma once

#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace scry::reflection::detail {

// Reflection reuses the public read-only JSON view instead of owning a second one.
using JsonKind = scry::JsonKind;
using JsonView = scry::JsonView;

[[nodiscard]] Result<JsonView> parse_json(const Json& json);
void append_json_string(std::string& output, std::string_view value);
[[nodiscard]] Result<Json> canonicalize_encoded_json(const Json& json);

} // namespace scry::reflection::detail
