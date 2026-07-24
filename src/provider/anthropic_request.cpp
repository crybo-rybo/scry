#include "provider/anthropic.hpp"
#include "provider/wire_json.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <variant>

namespace scry::detail {
namespace {

[[nodiscard]] Error invalid_request(std::string message) {
  return make_provider_error(ErrorCategory::invalid_config, std::move(message));
}

[[nodiscard]] Result<WireValue>
encode_boundary_json(const Json& json, const std::string_view failure_message) {
  return parse_wire_json(json.text, ErrorCategory::invalid_config, failure_message);
}

[[nodiscard]] Result<WireValue> encode_text(const TextBlock& block) {
  WireValue value{};
  value["type"] = "text";
  value["text"] = block.text;
  return value;
}

[[nodiscard]] Result<WireValue> encode_tool_call(const ToolCallBlock& block) {
  auto input = encode_boundary_json(block.arguments, "Tool input is not valid JSON");
  if (!input) {
    return std::unexpected(std::move(input.error()));
  }

  WireValue value{};
  value["type"] = "tool_use";
  value["id"] = block.id;
  value["name"] = block.name;
  value["input"] = std::move(*input);
  return value;
}

[[nodiscard]] Result<WireValue> encode_tool_result(const ToolResultBlock& block) {
  auto result = encode_boundary_json(block.result, "Tool result is not valid JSON");
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  auto encoded = write_wire_json(*result, ErrorCategory::invalid_config,
                                 "Tool result could not be encoded");
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }

  WireValue value{};
  value["type"] = "tool_result";
  value["tool_use_id"] = block.tool_call_id;
  // Glaze's assignment operators bind const&, so moving into the node's
  // variant is what transfers the payload instead of duplicating it.
  value["content"].data = std::move(*encoded);
  value["is_error"] = block.is_error;
  return value;
}

[[nodiscard]] Result<WireValue> encode_content(const ContentBlock& block) {
  if (const auto* text = std::get_if<TextBlock>(&block)) {
    return encode_text(*text);
  }
  if (const auto* call = std::get_if<ToolCallBlock>(&block)) {
    return encode_tool_call(*call);
  }
  return encode_tool_result(std::get<ToolResultBlock>(block));
}

[[nodiscard]] Result<WireValue> encode_message(const Message& message) {
  WireValue::array_t content{};
  content.reserve(message.content.size());
  for (const auto& block : message.content) {
    auto encoded = encode_content(block);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    content.push_back(std::move(*encoded));
  }

  WireValue value{};
  value["role"] = message.role == Role::user ? "user" : "assistant";
  value["content"].data = std::move(content);
  return value;
}

[[nodiscard]] Result<WireValue::array_t>
encode_messages(const std::vector<Message>& messages) {
  WireValue::array_t encoded{};
  encoded.reserve(messages.size());
  for (const auto& message : messages) {
    auto value = encode_message(message);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    encoded.push_back(std::move(*value));
  }
  return encoded;
}

[[nodiscard]] Result<WireValue> encode_tool(const ToolSchema& tool) {
  auto schema =
      encode_boundary_json(tool.input_schema, "Tool input schema is not valid JSON");
  if (!schema) {
    return std::unexpected(std::move(schema.error()));
  }

  WireValue value{};
  value["name"] = tool.name;
  value["description"] = tool.description;
  value["input_schema"] = std::move(*schema);
  return value;
}

[[nodiscard]] Result<WireValue::array_t>
encode_tools(const std::vector<ToolSchema>& tools) {
  WireValue::array_t encoded{};
  encoded.reserve(tools.size());
  for (const auto& tool : tools) {
    auto value = encode_tool(tool);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    encoded.push_back(std::move(*value));
  }
  return encoded;
}

[[nodiscard]] Status validate_sampling(const SamplingConfig& sampling) {
  if (!std::isfinite(sampling.temperature) || sampling.temperature < 0.0 ||
      sampling.temperature > 1.0) {
    return std::unexpected(
        invalid_request("Anthropic temperature must be between zero and one"));
  }
  if (sampling.top_p && (!std::isfinite(*sampling.top_p) || *sampling.top_p <= 0.0 ||
                         *sampling.top_p > 1.0)) {
    return std::unexpected(
        invalid_request("Anthropic top_p must be greater than zero and at most one"));
  }
  if (!sampling.max_tokens || *sampling.max_tokens == 0) {
    return std::unexpected(invalid_request("Anthropic max_tokens must be configured"));
  }
  return {};
}

[[nodiscard]] Status validate_request(const Config& config,
                                      const ModelRequest& request) {
  if (config.base_url.empty()) {
    return std::unexpected(invalid_request("Anthropic base_url is required"));
  }
  if (config.api_key.empty()) {
    return std::unexpected(invalid_request("Anthropic api_key is required"));
  }
  if (request.model.empty() && config.model.empty()) {
    return std::unexpected(invalid_request("Anthropic model is required"));
  }
  return validate_sampling(request.sampling);
}

[[nodiscard]] std::string endpoint(std::string base_url) {
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  constexpr auto path = std::string_view{"/v1/messages"};
  if (!base_url.ends_with(path)) {
    base_url.append(path);
  }
  return base_url;
}

[[nodiscard]] Result<WireValue> make_request_body(const Config& config,
                                                  const ModelRequest& request) {
  auto messages = encode_messages(request.messages);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  auto tools = encode_tools(request.tools);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }

  WireValue root{};
  root["model"] = request.model.empty() ? config.model : request.model;
  root["max_tokens"] = request.sampling.max_tokens.value_or(0);
  root["temperature"] = request.sampling.temperature;
  root["stream"] = true;
  root["messages"].data = std::move(*messages);
  if (!request.system_prompt.empty()) {
    root["system"] = request.system_prompt;
  }
  if (request.sampling.top_p) {
    root["top_p"] = *request.sampling.top_p;
  }
  if (!tools->empty()) {
    root["tools"].data = std::move(*tools);
  }
  return root;
}

} // namespace

Result<TransportRequest>
AnthropicAdapter::make_request(const Config& config,
                               const ModelRequest& request) const {
  auto status = validate_request(config, request);
  if (!status) {
    return std::unexpected(std::move(status.error()));
  }
  auto body = make_request_body(config, request);
  if (!body) {
    return std::unexpected(std::move(body.error()));
  }
  auto encoded = write_wire_json(*body, ErrorCategory::invalid_config,
                                 "Anthropic request body could not be encoded");
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }

  return TransportRequest{
      .url = endpoint(config.base_url),
      .headers =
          {
              HttpHeader{.name = "content-type", .value = "application/json"},
              HttpHeader{.name = "x-api-key", .value = config.api_key},
              HttpHeader{.name = "anthropic-version", .value = "2023-06-01"},
              HttpHeader{.name = "accept", .value = "text/event-stream"},
          },
      .body = std::move(*encoded),
      .tls_verify_peer = config.tls_verify_peer,
      .timeouts = config.timeouts,
      .limits = config.limits,
  };
}

} // namespace scry::detail
