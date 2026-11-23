#include "kernel/kernel.hpp"

#include "kernel/json_text.hpp"

#include "kernel/error.hpp"

#include <scrydantic/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace scry::detail {
namespace {

// Whitespace per RFC 8259, the only kind the parser skips.
constexpr std::string_view json_whitespace = " \t\n\r";

// The caller's failure, followed by why and where `input` stopped being JSON. This
// reads the text again, which only a rejection pays for.
[[nodiscard]] Error json_text_error(const std::string_view input,
                                    const ErrorCategory category,
                                    const std::string_view failure_message) {
  std::string message{failure_message};
  if (const auto view = scrydantic::JsonView::parse(input); !view) {
    message.append(": ").append(view.error().message);
  }
  return make_error(category, std::move(message));
}

} // namespace

Status validate_json(const std::string_view input, const ErrorCategory category,
                     const std::string_view failure_message) {
  if (!scrydantic::is_valid_json(input)) {
    return std::unexpected(json_text_error(input, category, failure_message));
  }
  return {};
}

Status validate_json_object(const std::string_view input, const ErrorCategory category,
                            const std::string_view failure_message) {
  // A valid document starts, after whitespace, with its root's first byte.
  if (!scrydantic::is_valid_json(input) || !json_root_is_object(input)) {
    return std::unexpected(json_text_error(input, category, failure_message));
  }
  return {};
}

bool json_root_is_object(const std::string_view text) noexcept {
  const auto first = text.find_first_not_of(json_whitespace);
  return first != std::string_view::npos && text[first] == '{';
}

Result<scrydantic::JsonView> parse_json_object(const std::string_view text,
                                               const ErrorCategory category,
                                               const std::string_view failure_message) {
  auto view = scrydantic::JsonView::parse(text);
  if (!view || view->kind() != scrydantic::JsonKind::object) {
    return std::unexpected(json_text_error(text, category, failure_message));
  }
  return std::move(*view);
}

Result<scrydantic::Json> canonicalize_json(const scrydantic::Json& json,
                                           const ErrorCategory category,
                                           const std::string_view failure_message) {
  const auto view = scrydantic::JsonView::parse(json);
  if (!view) {
    return std::unexpected(json_text_error(json.text, category, failure_message));
  }
  return view->to_json();
}

Result<scrydantic::Json>
canonicalize_json_object(const scrydantic::Json& json, const ErrorCategory category,
                         const std::string_view failure_message) {
  return parse_json_object(json.text, category, failure_message)
      .transform([](const scrydantic::JsonView& view) { return view.to_json(); });
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
