#include "kernel/kernel.hpp"

#include "kernel/json/codec.hpp"

#include "kernel/error.hpp"
#include "kernel/json/document.hpp"
#include "kernel/json/number.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace scry::detail {
namespace {

// Whitespace per RFC 8259, the only kind the parser skips.
constexpr std::string_view json_whitespace = " \t\n\r";

[[nodiscard]] Error codec_error(const ErrorCategory category,
                                const std::string_view failure_message) {
  return make_error(category, std::string{failure_message});
}

// The canonical text of a document, or empty when the text is not one JSON
// document or, with `object_only`, when its root is not an object.
[[nodiscard]] std::optional<std::string> canonical_text(const std::string_view text,
                                                        const bool object_only) {
  const auto document = json::parse(text);
  if (!document || (object_only && document->object() == nullptr)) {
    return std::nullopt;
  }
  return json::write(*document);
}

} // namespace

Status validate_json(const std::string_view input, const ErrorCategory category,
                     const std::string_view failure_message) {
  if (!json::validate(input)) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return {};
}

Status validate_json_object(const std::string_view input, const ErrorCategory category,
                            const std::string_view failure_message) {
  if (auto status = validate_json(input, category, failure_message); !status) {
    return status;
  }
  // A valid document starts, after whitespace, with its root's first byte.
  const auto first = input.find_first_not_of(json_whitespace);
  if (first == std::string_view::npos || input[first] != '{') {
    return std::unexpected(codec_error(category, failure_message));
  }
  return {};
}

Result<Json> canonicalize_json(const Json& json, const ErrorCategory category,
                               const std::string_view failure_message) {
  auto text = canonical_text(json.text, false);
  if (!text) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return Json{.text = std::move(*text)};
}

Result<Json> canonicalize_json_object(const Json& json, const ErrorCategory category,
                                      const std::string_view failure_message) {
  auto text = canonical_text(json.text, true);
  if (!text) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return Json{.text = std::move(*text)};
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

} // namespace scry::detail
