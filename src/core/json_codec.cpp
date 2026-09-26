#include "core/json_codec.hpp"

#include "core/error.hpp"

#include <utility>

namespace scry::detail {
namespace {

// Input always arrives as a string_view, so Glaze reads it without a NUL sentinel
// and reports a value that ended with the buffer as the non-error code
// `end_reached`. validate_trailing_whitespace is what rejects trailing garbage and
// a second document; the read itself rejects a truncated scalar.
struct JsonReadOptions : glz::opts {
  bool validate_trailing_whitespace = true;
};
constexpr JsonReadOptions json_read_options{{.null_terminated = false}};

// Whitespace per RFC 8259. A buffer holding only these never held a document, but
// Glaze parses it as a bare null, so it is rejected before the read.
constexpr std::string_view json_whitespace = " \t\n\r";

// The variant reader behind glz::generic clears Glaze's `end_reached` code before
// the top level settles it into success or truncation, so a buffer that stops
// inside a container (`{"a":1`, `[1,2`, `{"a"`) reads as a whole document.
// Completion is therefore judged directly: nesting closed (depth back at zero)
// and the input held something other than whitespace.
[[nodiscard]] bool document_is_complete(const std::string_view input,
                                        const glz::context& context) noexcept {
  return context.depth == 0 &&
         input.find_first_not_of(json_whitespace) != std::string_view::npos;
}

// Glaze's validating reader with no destination: it checks every byte of the
// document and allocates nothing, which is what lets a request encoder splice
// stored text after one pass instead of rebuilding it as a tree.
struct JsonSkipOptions : glz::opts {
  bool validate_skipped = true;
  bool validate_trailing_whitespace = true;
};
constexpr JsonSkipOptions json_skip_options{{.null_terminated = false}};

[[nodiscard]] Error field_error(const std::string_view name,
                                const std::string_view expected) {
  return make_error(ErrorCategory::protocol, "JSON field '" + std::string{name} +
                                                 "' must be " + std::string{expected});
}

} // namespace

Status parse_json_into(JsonValue& destination, const std::string_view input,
                       const ErrorCategory category,
                       const std::string_view failure_message) {
  glz::context context{};
  if (glz::read<json_read_options>(destination, input, context) ||
      !document_is_complete(input, context)) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return {};
}

Result<JsonValue> parse_json(const std::string_view input, const ErrorCategory category,
                             const std::string_view failure_message) {
  JsonValue value{};
  return parse_json_into(value, input, category, failure_message).transform([&value] {
    return std::move(value);
  });
}

Result<std::string> write_json_text(const JsonValue& value,
                                    const ErrorCategory category,
                                    const std::string_view failure_message) {
  auto encoded = glz::write<json_write_options>(value);
  if (!encoded) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return std::move(*encoded);
}

Result<Json> write_json(const JsonValue& value, const ErrorCategory category,
                        const std::string_view failure_message) {
  return write_json_text(value, category, failure_message)
      .transform([](std::string text) { return Json{.text = std::move(text)}; });
}

Result<Json> canonicalize_json(const Json& json, const ErrorCategory category,
                               const std::string_view failure_message) {
  return parse_json(json.text, category, failure_message)
      .and_then([&](const JsonValue& value) {
        return write_json(value, category, failure_message);
      });
}

Result<Json> canonicalize_json_object(const Json& json, const ErrorCategory category,
                                      const std::string_view failure_message) {
  return parse_json(json.text, category, failure_message)
      .and_then([&](const JsonValue& value) -> Result<Json> {
        if (!value.is_object()) {
          return std::unexpected(make_error(category, std::string{failure_message}));
        }
        return write_json(value, category, failure_message);
      });
}

Json make_json_error_object(const std::string_view message) {
  // Writing one string into a growable buffer cannot fail.
  std::string quoted{};
  static_cast<void>(glz::write<json_write_options>(message, quoted));
  return Json{.text = "{\"error\":" + quoted + "}"};
}

const JsonValue* json_field(const JsonValue& value,
                            const std::string_view name) noexcept {
  if (!value.is_object()) {
    return nullptr;
  }
  const auto& object = value.get_object();
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

Result<std::string_view> required_json_string(const JsonValue& value,
                                              const std::string_view name) {
  const auto* field = json_field(value, name);
  if (field == nullptr || !field->is_string()) {
    return std::unexpected(field_error(name, "a string"));
  }
  return field->get_string();
}

Result<const JsonValue::array_t*> required_json_array(const JsonValue& value,
                                                      const std::string_view name) {
  const auto* field = json_field(value, name);
  if (field == nullptr || !field->is_array()) {
    return std::unexpected(field_error(name, "an array"));
  }
  return &field->get_array();
}

Result<const JsonValue*> required_json_object(const JsonValue& value,
                                              const std::string_view name) {
  const auto* field = json_field(value, name);
  if (field == nullptr || !field->is_object()) {
    return std::unexpected(field_error(name, "an object"));
  }
  return field;
}

Result<std::optional<std::string_view>>
optional_json_string(const JsonValue& value, const std::string_view name) {
  const auto* field = json_field(value, name);
  if (field == nullptr || field->is_null()) {
    return std::optional<std::string_view>{};
  }
  if (!field->is_string()) {
    return std::unexpected(field_error(name, "a string or null"));
  }
  return std::optional<std::string_view>{field->get_string()};
}

Result<std::optional<std::uint64_t>> optional_json_uint(const JsonValue& value,
                                                        const std::string_view name) {
  const auto* field = json_field(value, name);
  if (field == nullptr || field->is_null()) {
    return std::optional<std::uint64_t>{};
  }
  if (!field->is_uint64()) {
    return std::unexpected(field_error(name, "an unsigned integer"));
  }
  return std::optional<std::uint64_t>{field->get<std::uint64_t>()};
}

Status validate_json(const std::string_view input, const ErrorCategory category,
                     const std::string_view failure_message) {
  glz::skip skipped{};
  glz::context context{};
  if (glz::read<json_skip_options>(skipped, input, context)) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return {};
}

Status validate_json_object(const std::string_view input, const ErrorCategory category,
                            const std::string_view failure_message) {
  if (auto status = validate_json(input, category, failure_message); !status) {
    return status;
  }
  const auto first = input.find_first_not_of(json_whitespace);
  if (first == std::string_view::npos || input[first] != '{') {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return {};
}

} // namespace scry::detail
