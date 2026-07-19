#include "provider/openai.hpp"
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

[[nodiscard]] Result<WireValue> boundary_json_object(const Json& json,
                                                     const std::string_view name) {
  auto parsed = parse_wire_json(json.text, ErrorCategory::invalid_config,
                                "OpenAI " + std::string{name} + " is not valid JSON");
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (!parsed->is_object()) {
    return std::unexpected(
        invalid_request("OpenAI " + std::string{name} + " must be a JSON object"));
  }
  return parsed;
}

[[nodiscard]] Result<std::string> boundary_json_string(const Json& json,
                                                       const std::string_view name) {
  auto parsed = parse_wire_json(json.text, ErrorCategory::invalid_config,
                                "OpenAI " + std::string{name} + " is not valid JSON");
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  return write_wire_json(*parsed, ErrorCategory::invalid_config,
                         "OpenAI " + std::string{name} + " could not be encoded");
}

[[nodiscard]] Result<WireValue> encode_tool_call(const ToolCallBlock& call) {
  if (call.id.empty() || call.name.empty()) {
    return std::unexpected(
        invalid_request("OpenAI assistant tool calls require nonempty IDs and names"));
  }
  auto arguments = boundary_json_object(call.arguments, "tool arguments");
  if (!arguments) {
    return std::unexpected(std::move(arguments.error()));
  }
  auto encoded = write_wire_json(*arguments, ErrorCategory::invalid_config,
                                 "OpenAI tool arguments could not be encoded");
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }

  WireValue function{};
  function["name"] = call.name;
  function["arguments"] = *encoded;
  WireValue value{};
  value["id"] = call.id;
  value["type"] = "function";
  value["function"] = std::move(function);
  return value;
}

[[nodiscard]] Result<WireValue> encode_tool_result(const ToolResultBlock& result) {
  if (result.tool_call_id.empty()) {
    return std::unexpected(
        invalid_request("OpenAI tool results require a nonempty call ID"));
  }
  auto content = boundary_json_string(result.result, "tool result");
  if (!content) {
    return std::unexpected(std::move(content.error()));
  }
  WireValue value{};
  value["role"] = "tool";
  value["tool_call_id"] = result.tool_call_id;
  value["content"] = *content;
  return value;
}

[[nodiscard]] Result<std::vector<WireValue>>
encode_user_message(const Message& message) {
  std::string text{};
  std::vector<const ToolResultBlock*> results{};
  bool saw_text = false;
  for (const auto& block : message.content) {
    if (const auto* text_block = std::get_if<TextBlock>(&block)) {
      saw_text = true;
      text.append(text_block->text);
    } else if (const auto* result_block = std::get_if<ToolResultBlock>(&block)) {
      results.push_back(result_block);
    } else {
      return std::unexpected(
          invalid_request("OpenAI user messages cannot contain tool calls"));
    }
  }
  if (saw_text && !results.empty()) {
    return std::unexpected(
        invalid_request("OpenAI user messages cannot mix text and tool results"));
  }
  std::vector<WireValue> encoded{};
  if (results.empty()) {
    WireValue value{};
    value["role"] = "user";
    value["content"] = text;
    encoded.push_back(std::move(value));
    return encoded;
  }
  encoded.reserve(results.size());
  for (const auto* result : results) {
    auto value = encode_tool_result(*result);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    encoded.push_back(std::move(*value));
  }
  return encoded;
}

[[nodiscard]] Result<std::vector<WireValue>>
encode_assistant_message(const Message& message) {
  std::string text{};
  WireValue::array_t calls{};
  for (const auto& block : message.content) {
    if (const auto* text_block = std::get_if<TextBlock>(&block)) {
      text.append(text_block->text);
      continue;
    }
    const auto* call = std::get_if<ToolCallBlock>(&block);
    if (call == nullptr) {
      return std::unexpected(
          invalid_request("OpenAI assistant messages cannot contain tool results"));
    }
    auto encoded = encode_tool_call(*call);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    calls.push_back(std::move(*encoded));
  }

  WireValue value{};
  value["role"] = "assistant";
  if (text.empty() && !calls.empty()) {
    value["content"] = nullptr;
  } else {
    value["content"] = text;
  }
  if (!calls.empty()) {
    value["tool_calls"] = calls;
  }
  return std::vector<WireValue>{std::move(value)};
}

[[nodiscard]] Result<WireValue::array_t> encode_messages(const ModelRequest& request) {
  WireValue::array_t encoded{};
  if (!request.system_prompt.empty()) {
    WireValue system{};
    system["role"] = "system";
    system["content"] = request.system_prompt;
    encoded.push_back(std::move(system));
  }
  for (const auto& message : request.messages) {
    auto values = message.role == Role::user ? encode_user_message(message)
                                             : encode_assistant_message(message);
    if (!values) {
      return std::unexpected(std::move(values.error()));
    }
    for (auto& value : *values) {
      encoded.push_back(std::move(value));
    }
  }
  return encoded;
}

[[nodiscard]] Result<WireValue> encode_tool(const ToolSchema& tool) {
  if (tool.name.empty()) {
    return std::unexpected(invalid_request("OpenAI tools require a nonempty name"));
  }
  auto parameters = boundary_json_object(tool.input_schema, "tool schema");
  if (!parameters) {
    return std::unexpected(std::move(parameters.error()));
  }
  WireValue function{};
  function["name"] = tool.name;
  function["description"] = tool.description;
  function["parameters"] = std::move(*parameters);
  WireValue value{};
  value["type"] = "function";
  value["function"] = std::move(function);
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
      sampling.temperature > 2.0) {
    return std::unexpected(
        invalid_request("OpenAI temperature must be finite and between zero and two"));
  }
  if (sampling.top_p && (!std::isfinite(*sampling.top_p) || *sampling.top_p < 0.0 ||
                         *sampling.top_p > 1.0)) {
    return std::unexpected(
        invalid_request("OpenAI top_p must be finite and between zero and one"));
  }
  if (!sampling.max_tokens || *sampling.max_tokens == 0) {
    return std::unexpected(invalid_request("OpenAI max_tokens must be configured"));
  }
  return {};
}

[[nodiscard]] Status validate_request(const Config& config,
                                      const ModelRequest& request) {
  if (config.base_url.empty()) {
    return std::unexpected(invalid_request("OpenAI base_url is required"));
  }
  if (config.api_key.find_first_of("\r\n") != std::string::npos) {
    return std::unexpected(
        invalid_request("OpenAI api_key must contain no line breaks"));
  }
  if (request.model.empty() && config.model.empty()) {
    return std::unexpected(invalid_request("OpenAI model is required"));
  }
  return validate_sampling(request.sampling);
}

[[nodiscard]] std::string endpoint(std::string base_url) {
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  constexpr auto endpoint_path = std::string_view{"/v1/chat/completions"};
  constexpr auto version_path = std::string_view{"/v1"};
  if (base_url.ends_with(endpoint_path)) {
    return base_url;
  }
  base_url.append(base_url.ends_with(version_path) ? "/chat/completions"
                                                   : endpoint_path);
  return base_url;
}

[[nodiscard]] Result<WireValue> make_request_body(const Config& config,
                                                  const ModelRequest& request) {
  auto messages = encode_messages(request);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  auto tools = encode_tools(request.tools);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }
  WireValue root{};
  root["model"] = request.model.empty() ? config.model : request.model;
  root["messages"] = *messages;
  root["temperature"] = request.sampling.temperature;
  root["max_tokens"] = request.sampling.max_tokens.value_or(0);
  root["stream"] = request.streaming;
  if (request.sampling.top_p) {
    root["top_p"] = *request.sampling.top_p;
  }
  if (request.streaming) {
    WireValue stream_options{};
    stream_options["include_usage"] = true;
    root["stream_options"] = std::move(stream_options);
  }
  if (!tools->empty()) {
    root["tools"] = *tools;
  }
  return root;
}

[[nodiscard]] std::vector<HttpHeader> request_headers(const Config& config,
                                                      const bool streaming) {
  std::vector<HttpHeader> headers{
      HttpHeader{.name = "content-type", .value = "application/json"},
      HttpHeader{.name = "accept",
                 .value = streaming ? "text/event-stream" : "application/json"},
  };
  if (!config.api_key.empty()) {
    headers.push_back(HttpHeader{
        .name = "authorization",
        .value = "Bearer " + config.api_key,
    });
  }
  return headers;
}

} // namespace

Result<TransportRequest>
OpenAiAdapter::make_request(const Config& config, const ModelRequest& request) const {
  auto status = validate_request(config, request);
  if (!status) {
    return std::unexpected(std::move(status.error()));
  }
  auto body = make_request_body(config, request);
  if (!body) {
    return std::unexpected(std::move(body.error()));
  }
  auto encoded = write_wire_json(*body, ErrorCategory::invalid_config,
                                 "OpenAI request body could not be encoded");
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return TransportRequest{
      .url = endpoint(config.base_url),
      .headers = request_headers(config, request.streaming),
      .body = std::move(*encoded),
      .streaming = request.streaming,
      .tls_verify_peer = config.tls_verify_peer,
      .timeouts = config.timeouts,
      .limits = config.limits,
  };
}

} // namespace scry::detail
