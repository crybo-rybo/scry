#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/anthropic_error.hpp"
#include "provider/wire_json.hpp"

#include <string>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] ErrorCategory category_for_status(const std::int32_t status) {
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
    return "Anthropic authentication failed";
  case ErrorCategory::rate_limit:
    return "Anthropic rate limit exceeded";
  case ErrorCategory::network:
    return "Anthropic service returned a transient server error";
  default:
    return "Anthropic request was rejected";
  }
}

[[nodiscard]] std::string error_type(const WireValue& root) {
  const auto* error = wire_field(root, "error");
  if (error == nullptr || !error->is_object()) {
    return {};
  }
  const auto type = optional_wire_string(*error, "type");
  return type && *type ? sanitize_anthropic_error_type(**type) : std::string{};
}

[[nodiscard]] std::string response_identifier(const WireValue& root,
                                              std::string fallback) {
  if (!fallback.empty()) {
    return fallback;
  }
  auto identifier = optional_wire_string(root, "request_id");
  if (identifier && *identifier) {
    return std::string{**identifier};
  }
  return fallback;
}

[[nodiscard]] Error decode_http_error(const TransportResult& result,
                                      const std::string_view body) {
  const auto category = category_for_status(result.status_code);
  Error error = make_provider_error(category, message_for_status(category),
                                    category == ErrorCategory::rate_limit ||
                                        category == ErrorCategory::network);
  error.provider_request_id = result.provider_request_id;

  auto root = parse_wire_json(body, ErrorCategory::protocol,
                              "Anthropic error response is not valid JSON");
  if (root) {
    error.provider_request_id =
        response_identifier(*root, std::move(error.provider_request_id));
    const auto type = error_type(*root);
    if (!type.empty()) {
      error.provider_detail = "anthropic:" + type;
    }
  }
  return error;
}

[[nodiscard]] Result<std::vector<ContentBlock>> decode_content(const WireValue& root) {
  auto values = required_wire_array(root, "content");
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }

  std::vector<ContentBlock> content{};
  content.reserve((*values)->size());
  for (const auto& value : **values) {
    auto block = decode_anthropic_content(value, false);
    if (!block) {
      return std::unexpected(std::move(block.error()));
    }
    content.push_back(std::move(*block));
  }
  return content;
}

[[nodiscard]] Status validate_message_type(const WireValue& root) {
  auto type = required_wire_string(root, "type");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type != "message") {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "Anthropic response is not a Messages API message"));
  }
  return {};
}

[[nodiscard]] Result<ModelResponse> decode_success(const TransportResult& result,
                                                   const std::string_view body) {
  auto root = parse_wire_json(body, ErrorCategory::protocol,
                              "Anthropic response is not valid JSON");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  auto type_status = validate_message_type(*root);
  if (!type_status) {
    return std::unexpected(std::move(type_status.error()));
  }
  auto content = decode_content(*root);
  if (!content) {
    return std::unexpected(std::move(content.error()));
  }
  auto stop_reason = optional_wire_string(*root, "stop_reason");
  if (!stop_reason) {
    return std::unexpected(std::move(stop_reason.error()));
  }
  auto finish = decode_anthropic_finish(*stop_reason);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }

  ModelResponse response{
      .content = std::move(*content),
      .finish_reason = *finish,
      .provider_request_id = response_identifier(*root, result.provider_request_id),
  };
  auto usage = apply_anthropic_usage(*root, response.usage);
  if (!usage) {
    return std::unexpected(std::move(usage.error()));
  }
  return response;
}

} // namespace

Result<ModelResponse>
AnthropicAdapter::parse_response(const TransportResult& result,
                                 const std::string_view body) const {
  if (result.status_code < 200 || result.status_code >= 300) {
    return std::unexpected(decode_http_error(result, body));
  }
  return decode_success(result, body);
}

} // namespace scry::detail
