#include "json/core.hpp"

#include "json/document.hpp"
#include "json/number.hpp"

#include <cmath>
#include <optional>
#include <scrydantic/detail/json_text.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scrydantic::detail {
namespace {

// Whitespace per RFC 8259, the only kind the parser skips.
constexpr std::string_view json_whitespace = " \t\n\r";

// The canonical text of a document, or empty when the text is not one JSON
// document or, with `object_only`, when its root is not an object.
[[nodiscard]] std::optional<Json> canonical_text(const std::string_view text,
                                                 const bool object_only) {
  const auto document = json::parse(text);
  if (!document || (object_only && document->object() == nullptr)) {
    return std::nullopt;
  }
  return Json{.text = json::write(*document)};
}

} // namespace

bool validate_json(const std::string_view input) noexcept {
  return json::validate(input);
}

bool validate_json_object(const std::string_view input) noexcept {
  // A valid document starts, after whitespace, with its root's first byte.
  return json::validate(input) && json_root_is_object(input);
}

bool json_root_is_object(const std::string_view text) noexcept {
  const auto first = text.find_first_not_of(json_whitespace);
  return first != std::string_view::npos && text[first] == '{';
}

std::optional<Json> canonicalize_json(const Json& json) {
  return canonical_text(json.text, false);
}

std::optional<Json> canonicalize_json_object(const Json& json) {
  return canonical_text(json.text, true);
}

Json make_json_error_object(const std::string_view message) {
  std::string text{"{\"error\":"};
  json::append_quoted(text, message);
  text.push_back('}');
  return Json{.text = std::move(text)};
}

Json canonical_json_number(const double value) {
  if (!std::isfinite(value)) {
    return Json{.text = "null"};
  }
  std::string text{};
  json::append_double(text, value);
  return Json{.text = std::move(text)};
}

} // namespace scrydantic::detail
