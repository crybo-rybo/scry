#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/anthropic.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {

struct AnthropicTool {
  std::string_view name{};
  std::string_view description{};
  glz::raw_json_view input_schema{};
};

struct AnthropicRequestBody {
  std::string_view model{};
  std::uint32_t max_tokens{};
  double temperature{};
  bool stream{true};
  glz::raw_json_view messages{};
  std::optional<std::string_view> system{};
  std::optional<double> top_p{};
  std::optional<glz::raw_json_view> tools{};
};

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
  JsonValue value{};
  value["type"] = "tool_result";
  value["tool_use_id"] = block.tool_call_id;
  value["content"].data = block.result.text;
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

[[nodiscard]] Result<JsonValue::array_t>
encode_messages(const SharedHistory& messages) {
  JsonValue::array_t encoded{};
  encoded.reserve(messages.size());
  for (const auto& message : messages) {
    auto value = encode_message(*message);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    encoded.push_back(std::move(*value));
  }
  return encoded;
}

[[nodiscard]] Result<std::string> encode_json(const auto& value,
                                              const std::string_view what) {
  auto encoded = glz::write_json(value);
  if (!encoded) {
    return std::unexpected(
        make_error(ErrorCategory::invalid_config, std::string{what}));
  }
  return std::move(*encoded);
}

[[nodiscard]] Result<AnthropicTool> encode_tool(const ToolSchema& tool) {
  auto schema =
      encode_boundary_json(tool.input_schema, "Tool input schema is not valid JSON");
  if (!schema) {
    return std::unexpected(std::move(schema.error()));
  }
  return AnthropicTool{
      .name = tool.name,
      .description = tool.description,
      .input_schema = glz::raw_json_view{tool.input_schema.text},
  };
}

[[nodiscard]] Result<std::string> encode_tools(const std::vector<ToolSchema>& tools) {
  if (tools.empty()) {
    return std::string{};
  }
  std::vector<AnthropicTool> encoded{};
  encoded.reserve(tools.size());
  for (const auto& tool : tools) {
    auto value = encode_tool(tool);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    encoded.push_back(*value);
  }
  return encode_json(encoded, "Anthropic tools could not be encoded");
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

[[nodiscard]] Result<std::string> make_request_body(const Config& config,
                                                    const ModelRequest& request) {
  auto messages = encode_messages(request.messages);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  auto tools = encode_tools(tool_schemas(request));
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }
  auto encoded_messages =
      encode_json(*messages, "Anthropic messages could not be encoded");
  if (!encoded_messages) {
    return std::unexpected(std::move(encoded_messages.error()));
  }

  AnthropicRequestBody body{
      .model = config.model,
      .max_tokens = request.sampling.max_tokens.value_or(0),
      .temperature = request.sampling.temperature,
      .messages = glz::raw_json_view{*encoded_messages},
      .system = request.system_prompt.empty()
                    ? std::nullopt
                    : std::optional<std::string_view>{request.system_prompt},
      .top_p = request.sampling.top_p,
      .tools =
          tools->empty() ? std::nullopt : std::optional<glz::raw_json_view>{*tools},
  };
  return encode_json(body, "Anthropic request body could not be encoded");
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

  return TransportRequest{
      .url = endpoint(config.base_url),
      .headers =
          {
              HttpHeader{.name = "content-type", .value = "application/json"},
              HttpHeader{.name = "x-api-key", .value = config.api_key},
              HttpHeader{.name = "anthropic-version", .value = "2023-06-01"},
              HttpHeader{.name = "accept", .value = "text/event-stream"},
          },
      .body = std::move(*body),
      .tls_verify_peer = config.tls_verify_peer,
      .timeouts = config.timeouts,
      .limits = config.limits,
  };
}

} // namespace scry::detail
