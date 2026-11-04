#include "kernel/kernel.hpp"

#include "kernel/json/codec.hpp"

#include "kernel/error.hpp"

#include <scrydantic/detail/json_text.hpp>
#include <scrydantic/json.hpp>
#include <string>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error codec_error(const ErrorCategory category,
                                const std::string_view failure_message) {
  return make_error(category, std::string{failure_message});
}

// The canonical form of `json`, if it is one JSON document and, when
// `require_object` is set, its root is an object.
[[nodiscard]] Result<scrydantic::Json>
canonical_text(const scrydantic::Json& json, const bool require_object,
               const ErrorCategory category, const std::string_view failure_message) {
  const auto parsed = scrydantic::JsonView::parse(json);
  if (!parsed || (require_object && parsed->kind() != scrydantic::JsonKind::object)) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return parsed->to_json();
}

} // namespace

Status validate_json(const std::string_view input, const ErrorCategory category,
                     const std::string_view failure_message) {
  if (!scrydantic::detail::validate_json(input)) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return {};
}

Status validate_json_object(const std::string_view input, const ErrorCategory category,
                            const std::string_view failure_message) {
  if (!json_root_is_object(input) || !scrydantic::detail::validate_json(input)) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return {};
}

bool json_root_is_object(const std::string_view text) noexcept {
  const auto first = text.find_first_not_of(" \t\n\r");
  return first != std::string_view::npos && text[first] == '{';
}

Result<scrydantic::Json> canonicalize_json(const scrydantic::Json& json,
                                           const ErrorCategory category,
                                           const std::string_view failure_message) {
  return canonical_text(json, false, category, failure_message);
}

Result<scrydantic::Json>
canonicalize_json_object(const scrydantic::Json& json, const ErrorCategory category,
                         const std::string_view failure_message) {
  return canonical_text(json, true, category, failure_message);
}

scrydantic::Json make_json_error_object(const std::string_view message) {
  return {.text = "{\"error\":" + scrydantic::escape_json_string(message) + "}"};
}

scrydantic::Json canonical_json_number(const double value) {
  std::string text{};
  scrydantic::detail::append_canonical_json_number(text, value);
  return {.text = std::move(text)};
}

} // namespace scry::detail
