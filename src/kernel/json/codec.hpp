#pragma once

#include "kernel/error.hpp"

#include <scry/error.hpp>
#include <scry/json.hpp>
#include <string_view>

// The JSON codec's text-level operations, which the rest of src/ uses. The
// document type behind them stays inside the kernel (kernel/json/document.hpp,
// which states what is accepted and how canonical text is written); outside it,
// parsed JSON is read through the public JsonView and reflected types through
// the reflected codec.
namespace scry::detail {

// Validates JSON text without materializing a document: one allocation-free
// pass that accepts exactly what the parser accepts, rejecting malformed
// interiors, truncation, trailing garbage, and a second document. The object variant
// also requires the root to be an object. Surrounding whitespace is accepted, as any
// parser would.
[[nodiscard]] Status validate_json(std::string_view input, ErrorCategory category,
                                   std::string_view failure_message);

[[nodiscard]] Status validate_json_object(std::string_view input,
                                          ErrorCategory category,
                                          std::string_view failure_message);

// Whether the first byte after leading whitespace opens an object. Only that byte
// is read, so the rest of the text must be valid JSON already or be validated
// separately.
[[nodiscard]] bool json_root_is_object(std::string_view text) noexcept;

// Rewrites JSON text in canonical form: object keys in lexical order, no
// insignificant whitespace, and the canonical spelling of every number and
// string escape.
[[nodiscard]] Result<Json> canonicalize_json(const Json& json, ErrorCategory category,
                                             std::string_view failure_message);

[[nodiscard]] Result<Json> canonicalize_json_object(const Json& json,
                                                    ErrorCategory category,
                                                    std::string_view failure_message);

[[nodiscard]] Json make_json_error_object(std::string_view message);

// A double spelled as the canonical writer spells it. That is the shortest
// round-trip digits, as std::to_chars writes them, but laid out positionally for
// decimal exponents from -4 to 15 and otherwise as `1E-7`, where std::to_chars
// chooses by length and writes `1e-07`. A non-finite value is written as null.
[[nodiscard]] Json canonical_json_number(double value);

} // namespace scry::detail
