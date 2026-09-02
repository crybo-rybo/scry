#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/anthropic.hpp"

#include <string>
#include <utility>
#include <variant>

namespace scry::detail {
namespace {

[[nodiscard]] Result<JsonValue>
encode_boundary_json(const Json& json, const std::string_view failure_message) {
  return parse_json(json.text, ErrorCategory::invalid_config, failure_message);
}

[[nodiscard]] Result<JsonValue> encode_text(const TextBlock& block) {
  JsonValue value{};
  value["type"] = "text";
  value["text"] = block.text;
  return value;
}

[[nodiscard]] Result<JsonValue> encode_tool_call(const ToolCallBlock& block) {
  auto input = encode_boundary_json(block.arguments, "Tool input is not valid JSON");
  if (!input) {
    return std::unexpected(std::move(input.error()));
  }

  JsonValue value{};
  value["type"] = "tool_use";
  value["id"] = block.id;
  value["name"] = block.name;
  value["input"] = std::move(*input);
  return value;
}

[[nodiscard]] Result<JsonValue> encode_tool_result(const ToolResultBlock& block) {
  auto result = encode_boundary_json(block.result, "Tool result is not valid JSON");
  if (!result) {
    return std::unexpected(std::move(result.error()));
  }
  auto encoded = write_json_text(*result, ErrorCategory::invalid_config,
                                 "Tool result could not be encoded");
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }

  JsonValue value{};
  value["type"] = "tool_result";
  value["tool_use_id"] = block.tool_call_id;
  // Glaze's assignment operators bind const&, so moving into the node's
  // variant is what transfers the payload instead of duplicating it.
  value["content"].data = std::move(*encoded);
  value["is_error"] = block.is_error;
  return value;
}

[[nodiscard]] Result<JsonValue> encode_content(const ContentBlock& block) {
  if (const auto* text = std::get_if<TextBlock>(&block)) {
    return encode_text(*text);
  }
  if (const auto* call = std::get_if<ToolCallBlock>(&block)) {
    return encode_tool_call(*call);
  }
  return encode_tool_result(std::get<ToolResultBlock>(block));
}

[[nodiscard]] Result<JsonValue> encode_message(const Message& message) {
  JsonValue::array_t content{};
  content.reserve(message.content.size());
  for (const auto& block : message.content) {
    auto encoded = encode_content(block);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    content.push_back(std::move(*encoded));
  }

  JsonValue value{};
  value["role"] = message.role == Role::user ? "user" : "assistant";
  value["content"].data = std::move(content);
  return value;
}

[[nodiscard]] Status encode_message_into(JsonValue::array_t& encoded,
                                         const Message& message) {
  auto value = encode_message(message);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  encoded.push_back(std::move(*value));
  return {};
}

[[nodiscard]] Result<JsonValue::array_t> encode_messages(const ModelRequest& request) {
  JsonValue::array_t encoded{};
  encoded.reserve(request.message_count());
  if (request.history) {
    for (const auto& message : *request.history) {
      if (auto status = encode_message_into(encoded, message); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
  }
  for (const auto& message : request.messages) {
    if (auto status = encode_message_into(encoded, message); !status) {
      return std::unexpected(std::move(status.error()));
    }
  }
  return encoded;
}

[[nodiscard]] Result<JsonValue> encode_tool(const ToolSchema& tool) {
  auto schema =
      encode_boundary_json(tool.input_schema, "Tool input schema is not valid JSON");
  if (!schema) {
    return std::unexpected(std::move(schema.error()));
  }

  JsonValue value{};
  value["name"] = tool.name;
  value["description"] = tool.description;
  value["input_schema"] = std::move(*schema);
  return value;
}

[[nodiscard]] Result<JsonValue::array_t>
encode_tools(const std::vector<ToolSchema>& tools) {
  JsonValue::array_t encoded{};
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

[[nodiscard]] Result<JsonValue> make_request_body(const Config& config,
                                                  const ModelRequest& request) {
  auto messages = encode_messages(request);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }

  JsonValue root{};
  root["model"] = config.model;
  // Validation rejects an unset max_tokens for this dialect, so the optional is
  // always engaged here.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  root["max_tokens"] = *request.sampling.max_tokens;
  root["temperature"] = request.sampling.temperature;
  root["stream"] = true;
  root["messages"].data = std::move(*messages);
  if (!request.system_prompt.empty()) {
    root["system"] = request.system_prompt;
  }
  if (request.sampling.top_p) {
    root["top_p"] = *request.sampling.top_p;
  }
  if (request.tools && !request.tools->empty()) {
    auto tools = encode_tools(*request.tools);
    if (!tools) {
      return std::unexpected(std::move(tools.error()));
    }
    root["tools"].data = std::move(*tools);
  }
  return root;
}

} // namespace

// Config is immutable per Harness and validated once at Harness::create, so
// this adapter encodes the request without re-checking endpoint, auth, or
// sampling bounds.
Result<TransportRequest>
AnthropicAdapter::make_request(const Config& config,
                               const ModelRequest& request) const {
  auto body = make_request_body(config, request);
  if (!body) {
    return std::unexpected(std::move(body.error()));
  }
  auto encoded = write_json_text(*body, ErrorCategory::invalid_config,
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
      .provider_namespace = "anthropic",
      .tls_verify_peer = config.tls_verify_peer,
      .timeouts = config.timeouts,
      .limits = config.limits,
  };
}

} // namespace scry::detail
