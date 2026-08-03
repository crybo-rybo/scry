#include "provider/openai_content.hpp"

#include "provider/shared.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace scry::detail {
namespace {

// Deliberately divergent from the Anthropic adapter: OpenAI usage objects
// replace the running totals, so a missing field zeroes the count instead of
// preserving the previous value.
[[nodiscard]] Status assign_usage_count(const JsonValue& usage,
                                        const std::string_view field,
                                        std::uint64_t& destination) {
  auto count = optional_json_uint(usage, field);
  if (!count) {
    return std::unexpected(std::move(count.error()));
  }
  destination = count->value_or(0);
  return {};
}

[[nodiscard]] ErrorCategory error_category(const std::string_view token) noexcept {
  if (token == "authentication_error" || token == "permission_error" ||
      token == "invalid_api_key") {
    return ErrorCategory::authentication;
  }
  if (token == "rate_limit_error" || token == "rate_limit_exceeded" ||
      token == "insufficient_quota") {
    return ErrorCategory::rate_limit;
  }
  if (token == "server_error" || token == "api_error" || token == "overloaded_error") {
    return ErrorCategory::network;
  }
  return ErrorCategory::protocol;
}

struct ErrorDescriptor {
  std::string token;
  ErrorCategory category;
};

[[nodiscard]] std::optional<std::string>
string_error_token(const JsonValue& error, const std::string_view field) {
  const auto* value = json_field(error, field);
  if (value == nullptr || !value->is_string()) {
    return std::nullopt;
  }
  auto token = sanitize_error_token(value->get_string());
  if (token == "unknown_error") {
    return std::nullopt;
  }
  return token;
}

[[nodiscard]] std::optional<ErrorDescriptor>
recognized_descriptor(const std::optional<std::string>& token) {
  if (!token) {
    return std::nullopt;
  }
  const auto category = error_category(*token);
  if (category == ErrorCategory::protocol) {
    return std::nullopt;
  }
  return ErrorDescriptor{.token = *token, .category = category};
}

[[nodiscard]] ErrorDescriptor
fallback_descriptor(const JsonValue& error, const std::optional<std::string>& type,
                    const std::optional<std::string>& code) {
  if (type) {
    return ErrorDescriptor{.token = *type, .category = ErrorCategory::protocol};
  }
  if (code) {
    return ErrorDescriptor{.token = *code, .category = ErrorCategory::protocol};
  }
  const auto* numeric_code = json_field(error, "code");
  if (numeric_code != nullptr && numeric_code->is_uint64()) {
    return ErrorDescriptor{
        .token =
            sanitize_error_token(std::to_string(numeric_code->get<std::uint64_t>())),
        .category = ErrorCategory::protocol,
    };
  }
  return ErrorDescriptor{
      .token = "unknown_error",
      .category = ErrorCategory::protocol,
  };
}

[[nodiscard]] ErrorDescriptor error_descriptor(const JsonValue& root) {
  const auto* error = json_field(root, "error");
  if (error == nullptr || !error->is_object()) {
    return ErrorDescriptor{
        .token = "unknown_error",
        .category = ErrorCategory::protocol,
    };
  }
  const auto type = string_error_token(*error, "type");
  const auto code = string_error_token(*error, "code");
  if (auto recognized = recognized_descriptor(type)) {
    return *recognized;
  }
  if (auto recognized = recognized_descriptor(code)) {
    return *recognized;
  }
  return fallback_descriptor(*error, type, code);
}

[[nodiscard]] bool retryable_category(const ErrorCategory category) noexcept {
  return category == ErrorCategory::rate_limit || category == ErrorCategory::network;
}

} // namespace

Status validate_openai_arguments(const std::string_view arguments) {
  auto parsed = parse_json(arguments, ErrorCategory::protocol,
                           "OpenAI tool arguments are not valid JSON");
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (!parsed->is_object()) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI tool arguments must be a JSON object"));
  }
  return {};
}

Result<FinishReason>
decode_openai_finish(const std::optional<std::string_view> reason) {
  if (!reason) {
    return FinishReason::unknown;
  }
  if (*reason == "stop") {
    return FinishReason::completed;
  }
  if (*reason == "length") {
    return FinishReason::length;
  }
  if (*reason == "tool_calls") {
    return FinishReason::tool_use;
  }
  if (*reason == "function_call") {
    return std::unexpected(make_error(
        ErrorCategory::protocol,
        "OpenAI returned the unsupported deprecated function_call finish reason"));
  }
  return FinishReason::unknown;
}

Status apply_openai_usage(const JsonValue& owner, Usage& usage) {
  const auto* value = json_field(owner, "usage");
  if (value == nullptr || value->is_null()) {
    return {};
  }
  if (!value->is_object()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "OpenAI usage must be an object or null"));
  }
  auto input = assign_usage_count(*value, "prompt_tokens", usage.input_tokens);
  if (!input) {
    return input;
  }
  return assign_usage_count(*value, "completion_tokens", usage.output_tokens);
}

bool is_openai_error(const JsonValue& root) noexcept {
  return json_field(root, "error") != nullptr;
}

Error decode_openai_error(const JsonValue& root, const std::string_view message,
                          std::string request_id) {
  const auto descriptor = error_descriptor(root);
  Error error = make_error(descriptor.category, std::string{message},
                           retryable_category(descriptor.category));
  error.provider_detail = "openai:" + descriptor.token;
  error.provider_request_id = std::move(request_id);
  return error;
}

} // namespace scry::detail
