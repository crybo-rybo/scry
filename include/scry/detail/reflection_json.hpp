#pragma once

#include <scry/error.hpp>
#include <scrydantic/json.hpp>
#include <string_view>

// Non-template bridges from the header-only codec into the library's JSON codec,
// so no reflected header needs a third-party type.
namespace scry::reflection::detail {

[[nodiscard]] Result<scrydantic::Json>
canonicalize_encoded_json(const scrydantic::Json& json);

// One allocation-free validation scan of a spliced scrydantic::Json payload.
[[nodiscard]] bool is_valid_json_text(std::string_view text);

} // namespace scry::reflection::detail
