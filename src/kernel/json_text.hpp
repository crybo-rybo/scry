#pragma once

#include "kernel/error.hpp"

#include <cstddef>
#include <scry/error.hpp>
#include <scrydantic/json.hpp>
#include <string_view>

// Scry's text-level JSON operations, which the rest of src/ uses. Scry has no
// JSON implementation of its own: these are thin layers over Scrydantic's public
// C++23 JSON core (<scrydantic/json.hpp>) that report a failure as a Scry Error
// under the caller's category. Outside them, parsed JSON is read through
// scrydantic::JsonView and typed values through Scrydantic's codec.
namespace scry::detail {

// Validates JSON text without materializing a document; see
// scrydantic::is_valid_json. The object variant also requires the root to be an
// object.
[[nodiscard]] Status validate_json(std::string_view input, ErrorCategory category,
                                   std::string_view failure_message);

[[nodiscard]] Status validate_json_object(std::string_view input,
                                          ErrorCategory category,
                                          std::string_view failure_message);

// Whether the first byte after leading whitespace opens an object. Only that byte
// is read, so the rest of the text must be valid JSON already or be validated
// separately.
[[nodiscard]] bool json_root_is_object(std::string_view text) noexcept;

// The deepest nesting of arrays and objects in `text`, counting an empty one, or
// zero for a scalar. Only brackets outside strings are counted, so the text must
// be valid JSON already.
[[nodiscard]] std::size_t json_nesting_depth(std::string_view text) noexcept;

// Rewrites JSON text in canonical form, as scrydantic::JsonView::to_json() writes
// it: object keys in lexical order, no insignificant whitespace, and the
// canonical spelling of every number and string escape.
[[nodiscard]] Result<scrydantic::Json>
canonicalize_json(const scrydantic::Json& json, ErrorCategory category,
                  std::string_view failure_message);

[[nodiscard]] Result<scrydantic::Json>
canonicalize_json_object(const scrydantic::Json& json, ErrorCategory category,
                         std::string_view failure_message);

// `{"error":<message>}`, with the message quoted by scrydantic::escape_json_string.
[[nodiscard]] scrydantic::Json make_json_error_object(std::string_view message);

// A double spelled as the canonical writer spells it; see
// scrydantic::format_json_number. A non-finite value is written as null.
[[nodiscard]] scrydantic::Json canonical_json_number(double value);

} // namespace scry::detail
