#include "kernel/kernel.hpp"

#include "kernel/json_text.hpp"

#include "kernel/error.hpp"

#include <algorithm>
#include <cstddef>
#include <scrydantic/json.hpp>
#include <string>
#include <string_view>

namespace scry::detail {
namespace {

// Whitespace per RFC 8259, the only kind the parser skips.
constexpr std::string_view json_whitespace = " \t\n\r";

[[nodiscard]] Error json_text_error(const ErrorCategory category,
                                    const std::string_view failure_message) {
  return make_error(category, std::string{failure_message});
}

// The canonical text of `json`, or the caller's failure when the text is not one
// JSON document or, with `object_only`, when its root is not an object.
[[nodiscard]] Result<scrydantic::Json> canonical_text(const scrydantic::Json& json,
                                                      const bool object_only,
                                                      const ErrorCategory category,
                                                      const std::string_view failure) {
  const auto view = scrydantic::JsonView::parse(json);
  if (!view || (object_only && view->kind() != scrydantic::JsonKind::object)) {
    return std::unexpected(json_text_error(category, failure));
  }
  return view->to_json();
}

} // namespace

Status validate_json(const std::string_view input, const ErrorCategory category,
                     const std::string_view failure_message) {
  if (!scrydantic::is_valid_json(input)) {
    return std::unexpected(json_text_error(category, failure_message));
  }
  return {};
}

Status validate_json_object(const std::string_view input, const ErrorCategory category,
                            const std::string_view failure_message) {
  // A valid document starts, after whitespace, with its root's first byte.
  if (!scrydantic::is_valid_json(input) || !json_root_is_object(input)) {
    return std::unexpected(json_text_error(category, failure_message));
  }
  return {};
}

bool json_root_is_object(const std::string_view text) noexcept {
  const auto first = text.find_first_not_of(json_whitespace);
  return first != std::string_view::npos && text[first] == '{';
}

std::size_t json_nesting_depth(const std::string_view text) noexcept {
  std::size_t depth = 0;
  std::size_t deepest = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char character : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
    } else if (character == '"') {
      in_string = true;
    } else if (character == '[' || character == '{') {
      deepest = std::max(deepest, ++depth);
    } else if (character == ']' || character == '}') {
      --depth;
    }
  }
  return deepest;
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
  std::string text{"{\"error\":"};
  text.append(scrydantic::escape_json_string(message));
  text.push_back('}');
  return scrydantic::Json{.text = std::move(text)};
}

scrydantic::Json canonical_json_number(const double value) {
  return scrydantic::Json{.text = scrydantic::format_json_number(value)};
}

} // namespace scry::detail
