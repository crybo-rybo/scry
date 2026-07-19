#include "core/json_codec.hpp"

#include <string>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error codec_error(const ErrorCategory category,
                                const std::string_view message) {
  return Error{
      .category = category,
      .message = std::string{message},
  };
}

} // namespace

Result<JsonValue> parse_json(const std::string_view input, const ErrorCategory category,
                             const std::string_view failure_message) {
  JsonValue value{};
  if (glz::read_json(value, input)) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return value;
}

Result<Json> write_json(const JsonValue& value, const ErrorCategory category,
                        const std::string_view failure_message) {
  auto encoded = glz::write_json(value);
  if (!encoded) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return Json{.text = std::move(*encoded)};
}

Status require_json_object(const JsonValue& value, const ErrorCategory category,
                           const std::string_view failure_message) {
  if (!value.is_object()) {
    return std::unexpected(codec_error(category, failure_message));
  }
  return {};
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
  if (auto status = require_json_object(*value, category, failure_message); !status) {
    return std::unexpected(std::move(status.error()));
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

} // namespace scry::detail
