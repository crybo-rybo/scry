#include "provider/openai.hpp"
#include "provider/openai_content.hpp"
#include "provider/wire_json.hpp"

#include <string>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Result<const WireValue*> required_object(const WireValue& owner,
                                                       const std::string_view name) {
  const auto* value = wire_field(owner, name);
  if (value == nullptr || !value->is_object()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI payload field '" + std::string{name} + "' must be an object"));
  }
  return value;
}

[[nodiscard]] ErrorCategory category_for_status(const std::int32_t status) noexcept {
  if (status == 401 || status == 403) {
    return ErrorCategory::authentication;
  }
  if (status == 429) {
    return ErrorCategory::rate_limit;
  }
  if (status >= 500 && status <= 599) {
    return ErrorCategory::network;
  }
  return ErrorCategory::protocol;
}

[[nodiscard]] std::string message_for_status(const ErrorCategory category) {
  switch (category) {
  case ErrorCategory::authentication:
    return "OpenAI authentication failed";
  case ErrorCategory::rate_limit:
    return "OpenAI rate limit exceeded";
  case ErrorCategory::network:
    return "OpenAI-compatible service returned a transient server error";
  default:
    return "OpenAI-compatible request was rejected";
  }
}

[[nodiscard]] Error decode_http_error(const TransportResult& result,
                                      const std::string_view body) {
  const auto category = category_for_status(result.status_code);
  auto root = parse_wire_json(body, ErrorCategory::protocol,
                              "OpenAI error response is not valid JSON");
  WireValue fallback{};
  const auto& payload = root ? *root : fallback;
  auto error = decode_openai_error(payload, message_for_status(category),
                                   result.provider_request_id);
  error.category = category;
  error.retryable =
      category == ErrorCategory::rate_limit || category == ErrorCategory::network;
  return error;
}

[[nodiscard]] Status validate_response_type(const WireValue& root) {
  auto type = required_wire_string(root, "object");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type != "chat.completion") {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI response is not a Chat Completions object"));
  }
  return {};
}

[[nodiscard]] Result<const WireValue*> single_choice(const WireValue& root) {
  auto choices = required_wire_array(root, "choices");
  if (!choices) {
    return std::unexpected(std::move(choices.error()));
  }
  if ((*choices)->size() != 1) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI response must contain exactly one choice"));
  }
  const auto& choice = (*choices)->front();
  if (!choice.is_object()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI response choice must be an object"));
  }
  auto index = optional_wire_uint(choice, "index");
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (index->value_or(1) != 0) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI response choice index must be zero"));
  }
  return &choice;
}

[[nodiscard]] Status reject_legacy_and_refusal(const WireValue& message) {
  const auto* legacy = wire_field(message, "function_call");
  if (legacy != nullptr && !legacy->is_null()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI returned the unsupported deprecated function_call field"));
  }
  auto refusal = optional_wire_string(message, "refusal");
  if (!refusal) {
    return std::unexpected(std::move(refusal.error()));
  }
  if (!refusal->value_or(std::string_view{}).empty()) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI returned unsupported structured refusal content"));
  }
  return {};
}

[[nodiscard]] Status validate_assistant_message(const WireValue& message) {
  auto role = required_wire_string(message, "role");
  if (!role) {
    return std::unexpected(std::move(role.error()));
  }
  if (*role != "assistant") {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI response role must be assistant"));
  }
  return reject_legacy_and_refusal(message);
}

[[nodiscard]] Status append_message_text(const WireValue& message,
                                         std::vector<ContentBlock>& content) {
  const auto* text = wire_field(message, "content");
  if (text == nullptr || (!text->is_string() && !text->is_null())) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI response content must be a string or null"));
  }
  if (text->is_string()) {
    content.push_back(TextBlock{.text = std::string{text->get_string()}});
  }
  return {};
}

[[nodiscard]] Status append_message_tools(const WireValue& message,
                                          std::vector<ContentBlock>& content) {
  const auto* calls = wire_field(message, "tool_calls");
  if (calls == nullptr || calls->is_null()) {
    return {};
  }
  if (!calls->is_array()) {
    return std::unexpected(make_provider_error(ErrorCategory::protocol,
                                               "OpenAI tool_calls must be an array"));
  }
  content.reserve(content.size() + calls->get_array().size());
  for (const auto& value : calls->get_array()) {
    auto call = decode_openai_tool_call(value);
    if (!call) {
      return std::unexpected(std::move(call.error()));
    }
    content.push_back(std::move(*call));
  }
  return {};
}

[[nodiscard]] Result<std::vector<ContentBlock>>
decode_message_content(const WireValue& message) {
  auto valid = validate_assistant_message(message);
  if (!valid) {
    return std::unexpected(std::move(valid.error()));
  }
  std::vector<ContentBlock> content{};
  auto text = append_message_text(message, content);
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  auto tools = append_message_tools(message, content);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }
  return content;
}

[[nodiscard]] Result<ModelResponse> decode_success(const TransportResult& result,
                                                   const WireValue& root) {
  auto type = validate_response_type(root);
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  auto choice = single_choice(root);
  if (!choice) {
    return std::unexpected(std::move(choice.error()));
  }
  auto message = required_object(**choice, "message");
  if (!message) {
    return std::unexpected(std::move(message.error()));
  }
  auto content = decode_message_content(**message);
  if (!content) {
    return std::unexpected(std::move(content.error()));
  }
  auto reason = optional_wire_string(**choice, "finish_reason");
  if (!reason) {
    return std::unexpected(std::move(reason.error()));
  }
  if (!*reason) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI response must contain a non-null finish reason"));
  }
  auto finish = decode_openai_finish(*reason);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  ModelResponse response{
      .content = std::move(*content),
      .finish_reason = *finish,
      .provider_request_id = result.provider_request_id,
  };
  auto usage = apply_openai_usage(root, response.usage);
  if (!usage) {
    return std::unexpected(std::move(usage.error()));
  }
  return response;
}

} // namespace

Result<ModelResponse> OpenAiAdapter::parse_response(const TransportResult& result,
                                                    const std::string_view body) const {
  if (result.status_code < 200 || result.status_code >= 300) {
    return std::unexpected(decode_http_error(result, body));
  }
  auto root = parse_wire_json(body, ErrorCategory::protocol,
                              "OpenAI response is not valid JSON");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  if (is_openai_error(*root)) {
    return std::unexpected(
        decode_openai_error(*root, "OpenAI-compatible response returned an error",
                            result.provider_request_id));
  }
  return decode_success(result, *root);
}

} // namespace scry::detail
