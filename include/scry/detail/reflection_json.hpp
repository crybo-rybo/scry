#pragma once

#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>

namespace scry::reflection::detail {

// The component reuses the public read-only JSON view rather than owning a second
// one; these aliases keep the historical spelling for the generated code.
using JsonKind = scry::JsonKind;
using JsonView = scry::JsonView;

[[nodiscard]] Result<JsonView> parse_json(Json json);
void append_json_string(std::string& output, std::string_view value);
[[nodiscard]] Result<Json> canonicalize_encoded_json(Json json);

} // namespace scry::reflection::detail
