#pragma once

#include <optional>
#include <scrydantic/json.hpp>
#include <string_view>

// The JSON core's text-level operations, for the reflected codec and for
// libraries built on Scrydantic that word their own diagnostics. They are not
// part of the stable API. The document type behind them stays inside
// src/json/document.hpp, which states what is accepted and how canonical text is
// written; outside it, parsed JSON is read through the public JsonView.
namespace scrydantic::detail {

// Validates JSON text without materializing a document: one allocation-free pass
// that accepts exactly what the parser accepts, rejecting malformed interiors,
// truncation, trailing garbage, and a second document. The object variant also
// requires the root to be an object. Surrounding whitespace is accepted, as any
// parser would.
[[nodiscard]] bool validate_json(std::string_view input) noexcept;

[[nodiscard]] bool validate_json_object(std::string_view input) noexcept;

// Whether the first byte after leading whitespace opens an object. Only that byte
// is read, so the rest of the text must be valid JSON already or be validated
// separately.
[[nodiscard]] bool json_root_is_object(std::string_view text) noexcept;

// Rewrites JSON text in canonical form: object keys in lexical order, no
// insignificant whitespace, and the canonical spelling of every number and string
// escape. Empty when the text is not one JSON document or, for the object
// variant, when its root is not an object.
[[nodiscard]] std::optional<Json> canonicalize_json(const Json& json);

[[nodiscard]] std::optional<Json> canonicalize_json_object(const Json& json);

// `{"error":<message>}`, with the message quoted as canonical JSON.
[[nodiscard]] Json make_json_error_object(std::string_view message);

// A double spelled as the canonical writer spells it. That is the shortest
// round-trip digits, as std::to_chars writes them, but laid out positionally for
// decimal exponents from -4 to 15 and otherwise as `1E-7`, where std::to_chars
// chooses by length and writes `1e-07`. A non-finite value is written as null.
[[nodiscard]] Json canonical_json_number(double value);

} // namespace scrydantic::detail
