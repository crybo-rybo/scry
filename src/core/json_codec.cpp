#include "core/json_codec.hpp"

#include "core/error.hpp"

#include <utility>

namespace scry::detail {
namespace {

constexpr glz::opts json_read_options{.null_terminated = false};

// Every input arrives as a string_view, so the reader is told the buffer is not
// NUL-terminated — and Glaze then treats the end of the buffer as an implicit
// terminator, accepting documents that stop mid-value. Skipping the document
// once with both validations on is what rejects a truncated body, a second
// document, and trailing garbage before the real read runs. A whitespace-only
// input still reads as null; that residual gap is the caller's to reject.
struct JsonValidateOptions : glz::opts {
  bool validate_skipped = true;
  bool validate_trailing_whitespace = true;
};
constexpr JsonValidateOptions json_validate_options{
    {.null_terminated = false}, true, true};

[[nodiscard]] Error field_error(const std::string_view name,
                                const std::string_view expected) {
  return make_error(ErrorCategory::protocol, "JSON field '" + std::string{name} +
                                                 "' must be " + std::string{expected});
}

} // namespace

Status parse_json_into(JsonValue& destination, const std::string_view input,
                       const ErrorCategory category,
                       const std::string_view failure_message) {
  glz::skip skipped{};
  glz::context validate_context{};
  if (glz::read<json_validate_options>(skipped, input, validate_context)) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  if (glz::read<json_read_options>(destination, input)) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return {};
}

Result<JsonValue> parse_json(const std::string_view input, const ErrorCategory category,
                             const std::string_view failure_message) {
  JsonValue value{};
  if (auto status = parse_json_into(value, input, category, failure_message); !status) {
    return std::unexpected(std::move(status.error()));
  }
  return value;
}

Result<std::string> write_json_text(const JsonValue& value,
                                    const ErrorCategory category,
                                    const std::string_view failure_message) {
  auto encoded = glz::write_json(value);
  if (!encoded) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return std::move(*encoded);
}

Result<Json> write_json(const JsonValue& value, const ErrorCategory category,
                        const std::string_view failure_message) {
  auto encoded = write_json_text(value, category, failure_message);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return Json{.text = std::move(*encoded)};
}

Result<Json> canonicalize_json(const Json& json, const ErrorCategory category,
                               const std::string_view failure_message) {
  auto value = parse_json(json.text, category, failure_message);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  return write_json(*value, category, failure_message);
}

Result<Json> canonicalize_json_object(const Json& json, const ErrorCategory category,
                                      const std::string_view failure_message) {
  auto value = parse_json(json.text, category, failure_message);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  if (!value->is_object()) {
    return std::unexpected(make_error(category, std::string{failure_message}));
  }
  return write_json(*value, category, failure_message);
}

Json make_json_error_object(const std::string_view message) {
  JsonValue value{};
  value["error"] = message;
  auto encoded = glz::write_json(value);
  if (!encoded) {
    return Json{.text = R"({"error":"tool execution failed"})"};
  }
  return Json{.text = std::move(*encoded)};
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

} // namespace scry::detail
