#pragma once

#include "kernel/error.hpp"

#include <scry/error.hpp>
#include <scrydantic/json.hpp>
#include <string_view>

// The JSON codec's text-level operations, which the rest of src/ uses. They report
// a failure as a Scry Error under the caller's category, and they are built on
// Scrydantic's public JsonView and escape_json_string() plus the two JSON core
// operations that have no public form, validate_json() and
// append_canonical_json_number() from <scrydantic/detail/json_text.hpp>. Outside
// them, parsed JSON is read through scrydantic::JsonView and reflected types
// through the reflected codec.
// TODO(https://github.com/crybo-rybo/scrydantic/issues/6): use public forms of
// validate_json() and append_canonical_json_number() once Scrydantic has them.
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
[[nodiscard]] bool json_root_is_object(std::string_view text) noexcept;

// Rewrites JSON text in canonical form, as scrydantic::JsonView::to_json() writes
// it: object keys in lexical order, no insignificant whitespace, and the
// canonical spelling of every number and string escape.
[[nodiscard]] Result<scrydantic::Json>
canonicalize_json(const scrydantic::Json& json, ErrorCategory category,
                  std::string_view failure_message);

[[nodiscard]] Result<scrydantic::Json>
canonicalize_json_object(const scrydantic::Json& json, ErrorCategory category,
                         std::string_view failure_message);

// `{"error":<message>}`, with the message quoted as canonical JSON.
[[nodiscard]] scrydantic::Json make_json_error_object(std::string_view message);

// A double spelled as the canonical writer spells it, such as `1`, `-0`, or
// `1E-7`; see scrydantic::detail::append_canonical_json_number. A non-finite
// value, which JSON cannot spell, is written as null.
[[nodiscard]] scrydantic::Json canonical_json_number(double value);

} // namespace scry::detail
