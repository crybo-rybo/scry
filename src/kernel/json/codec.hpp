#pragma once

#include "kernel/error.hpp"

#include <scry/error.hpp>
#include <scrydantic/detail/json_text.hpp>
#include <scrydantic/json.hpp>
#include <string_view>

// The JSON codec's text-level operations, which the rest of src/ uses. They are
// thin layers over Scrydantic's JSON core (<scrydantic/detail/json_text.hpp>)
// that report a failure as a Scry Error under the caller's category; outside
// them, parsed JSON is read through scrydantic::JsonView and reflected types
// through the reflected codec.
namespace scry::detail {

// Validates JSON text without materializing a document; see
// scrydantic::detail::validate_json. The object variant also requires the root
// to be an object.
[[nodiscard]] Status validate_json(std::string_view input, ErrorCategory category,
                                   std::string_view failure_message);

[[nodiscard]] Status validate_json_object(std::string_view input,
                                          ErrorCategory category,
                                          std::string_view failure_message);

// Whether the first byte after leading whitespace opens an object. Only that byte
// is read, so the rest of the text must be valid JSON already or be validated
// separately.
using scrydantic::detail::json_root_is_object;

// Rewrites JSON text in canonical form: object keys in lexical order, no
// insignificant whitespace, and the canonical spelling of every number and
// string escape.
[[nodiscard]] Result<scrydantic::Json>
canonicalize_json(const scrydantic::Json& json, ErrorCategory category,
                  std::string_view failure_message);

[[nodiscard]] Result<scrydantic::Json>
canonicalize_json_object(const scrydantic::Json& json, ErrorCategory category,
                         std::string_view failure_message);

using scrydantic::detail::make_json_error_object;

// A double spelled as the canonical writer spells it; see
// scrydantic::detail::canonical_json_number.
using scrydantic::detail::canonical_json_number;

} // namespace scry::detail
