#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/openai.hpp"
#include "provider/shared.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {

// The Chat Completions request, described as plain aggregates instead of a JSON
// tree. Glaze reflects each one member by member in declaration order, so the
// members are declared alphabetically and the body still leaves the encoder in
// the codec's canonical key order. Every std::string_view borrows from the
// Config or the ModelRequest, both of which outlive the encode, and every
// JsonText splices stored canonical JSON verbatim rather than re-parsing it.
//
// The names are dialect-qualified and the types deliberately sit outside the
// unnamed namespace. Glaze derives each member's name from a pointer into an
// `extern` object of the type, which a type with no linkage cannot have, and
// GCC mangles every translation unit's unnamed namespace identically, so two
// same-named wire structs here and in anthropic_request.cpp would have their key
// tables merged by the linker and each dialect would serialize with the
// other's keys.

// An assistant message with tool calls and no text sends `"content": null`, a
// distinct wire value from an omitted member, so the null is an alternative
// here rather than a disengaged optional. One text block is borrowed; several
// are joined into the owned alternative.
using OpenAiContent = std::variant<std::string_view, std::string, std::monostate>;

// `arguments` is a JSON string holding the argument document, not the document
// itself, which is why it is a string_view and `parameters` below is JsonText.
struct OpenAiCallFunction {
  std::string_view arguments{};
  std::string_view name{};
};

struct OpenAiToolCall {
  OpenAiCallFunction function{};
  std::string_view id{};
  std::string_view type{"function"};
};

struct OpenAiMessage {
  OpenAiContent content{};
  std::string_view role{};
  std::optional<std::string_view> tool_call_id{};
  std::optional<std::vector<OpenAiToolCall>> tool_calls{};
};

struct OpenAiToolFunction {
  std::string_view description{};
  std::string_view name{};
  JsonText parameters{};
};

struct OpenAiTool {
  OpenAiToolFunction function{};
  std::string_view type{"function"};
};

struct OpenAiStreamOptions {
  bool include_usage{true};
};

struct OpenAiBody {
  std::optional<std::uint32_t> max_tokens{};
  std::vector<OpenAiMessage> messages{};
  std::string_view model{};
  std::optional<std::string_view> reasoning_effort{};
  bool stream{true};
  OpenAiStreamOptions stream_options{};
  double temperature{};
  std::optional<std::vector<OpenAiTool>> tools{};
  std::optional<double> top_p{};
};

namespace {

[[nodiscard]] Error invalid_request(std::string message) {
  return make_error(ErrorCategory::invalid_config, std::move(message));
}

// Concatenates the message's text blocks. The single-block case - what a turn
// actually commits - borrows the block's bytes; anything else is joined once
// into a string the wire message owns.
[[nodiscard]] OpenAiContent message_text(const Message& message) {
  const TextBlock* single = nullptr;
  std::size_t blocks = 0;
  std::size_t bytes = 0;
  for (const auto& block : message.content) {
    if (const auto* text = std::get_if<TextBlock>(&block)) {
      single = text;
      ++blocks;
      bytes += text->text.size();
    }
  }
  if (blocks == 1) {
    return std::string_view{single->text};
  }
  std::string joined{};
  joined.reserve(bytes);
  for (const auto& block : message.content) {
    if (const auto* text = std::get_if<TextBlock>(&block)) {
      joined.append(text->text);
    }
  }
  return joined;
}

[[nodiscard]] bool text_is_empty(const OpenAiContent& content) noexcept {
  if (const auto* borrowed = std::get_if<std::string_view>(&content)) {
    return borrowed->empty();
  }
  if (const auto* owned = std::get_if<std::string>(&content)) {
    return owned->empty();
  }
  return true;
}

[[nodiscard]] Result<OpenAiToolCall> encode_tool_call(const ToolCallBlock& call) {
  if (call.id.empty() || call.name.empty()) {
    return std::unexpected(
        invalid_request("OpenAI assistant tool calls require nonempty IDs and names"));
  }
  if (auto status = embedded_json_object(call.arguments.text,
                                         "OpenAI tool arguments must be a JSON object");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return OpenAiToolCall{
      .function =
          OpenAiCallFunction{.arguments = call.arguments.text, .name = call.name},
      .id = call.id,
  };
}

[[nodiscard]] Result<OpenAiMessage> encode_tool_result(const ToolResultBlock& result) {
  if (result.tool_call_id.empty()) {
    return std::unexpected(
        invalid_request("OpenAI tool results require a nonempty call ID"));
  }
  if (auto status = embedded_json_value(result.result.text,
                                        "OpenAI tool result must be valid JSON");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return OpenAiMessage{
      .content = std::string_view{result.result.text},
      .role = "tool",
      .tool_call_id = std::string_view{result.tool_call_id},
  };
}

[[nodiscard]] Result<std::vector<OpenAiMessage>>
encode_user_message(const Message& message) {
  std::vector<const ToolResultBlock*> results{};
  bool saw_text = false;
  for (const auto& block : message.content) {
    if (std::holds_alternative<TextBlock>(block)) {
      saw_text = true;
    } else if (const auto* result = std::get_if<ToolResultBlock>(&block)) {
      results.push_back(result);
    } else {
      return std::unexpected(
          invalid_request("OpenAI user messages cannot contain tool calls"));
    }
  }
  if (saw_text && !results.empty()) {
    return std::unexpected(
        invalid_request("OpenAI user messages cannot mix text and tool results"));
  }
  std::vector<OpenAiMessage> encoded{};
  if (results.empty()) {
    encoded.push_back(OpenAiMessage{.content = message_text(message), .role = "user"});
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

[[nodiscard]] Result<std::vector<OpenAiMessage>>
encode_assistant_message(const Message& message) {
  std::vector<OpenAiToolCall> calls{};
  for (const auto& block : message.content) {
    if (std::holds_alternative<TextBlock>(block)) {
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
    // A wire tool call borrows every byte it carries, so it is trivially
    // copyable and there is nothing for a move to steal.
    calls.push_back(*encoded);
  }

  OpenAiMessage value{.content = message_text(message), .role = "assistant"};
  if (!calls.empty()) {
    if (text_is_empty(value.content)) {
      value.content = std::monostate{};
    }
    value.tool_calls = std::move(calls);
  }
  return std::vector<OpenAiMessage>{std::move(value)};
}

[[nodiscard]] Status encode_message_into(std::vector<OpenAiMessage>& encoded,
                                         const Message& message) {
  auto values = message.role == Role::user ? encode_user_message(message)
                                           : encode_assistant_message(message);
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }
  for (auto& value : *values) {
    encoded.push_back(std::move(value));
  }
  return {};
}

[[nodiscard]] Result<std::vector<OpenAiMessage>>
encode_messages(const ModelRequest& request) {
  std::vector<OpenAiMessage> encoded{};
  encoded.reserve(request.message_count() + 1U);
  if (!request.system_prompt.empty()) {
    encoded.push_back(OpenAiMessage{
        .content = std::string_view{request.system_prompt},
        .role = "system",
    });
  }
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

[[nodiscard]] Result<std::vector<OpenAiTool>>
encode_tools(const std::vector<ToolDefinition>& tools) {
  std::vector<OpenAiTool> encoded{};
  encoded.reserve(tools.size());
  for (const auto& tool : tools) {
    if (tool.name.empty()) {
      return std::unexpected(invalid_request("OpenAI tools require a nonempty name"));
    }
    if (auto status = embedded_json_object(tool.input_schema.text,
                                           "OpenAI tool schema must be a JSON object");
        !status) {
      return std::unexpected(std::move(status.error()));
    }
    encoded.push_back(OpenAiTool{
        .function =
            OpenAiToolFunction{
                .description = tool.description,
                .name = tool.name,
                .parameters = JsonText{tool.input_schema.text},
            },
    });
  }
  return encoded;
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

[[nodiscard]] Result<std::string> make_request_body(const Config& config,
                                                    const ModelRequest& request) {
  auto messages = encode_messages(request);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  std::optional<std::vector<OpenAiTool>> tools{};
  if (request.tools && !request.tools->empty()) {
    auto encoded = encode_tools(*request.tools);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    tools = std::move(*encoded);
  }

  const OpenAiBody body{
      .max_tokens = request.sampling.max_tokens,
      .messages = std::move(*messages),
      .model = config.model,
      .reasoning_effort = config.reasoning_mode == ReasoningMode::disabled
                              ? std::optional<std::string_view>{"none"}
                              : std::nullopt,
      .temperature = request.sampling.temperature,
      .tools = std::move(tools),
      .top_p = request.sampling.top_p,
  };
  return write_wire_json(body, ErrorCategory::invalid_config,
                         "OpenAI request body could not be encoded");
}

[[nodiscard]] std::vector<HttpHeader> request_headers(const Config& config) {
  std::vector<HttpHeader> headers{
      HttpHeader{.name = "content-type", .value = "application/json"},
      HttpHeader{.name = "accept", .value = "text/event-stream"},
  };
  if (!config.api_key.empty()) {
    headers.push_back(HttpHeader{
        .name = "authorization",
        .value = "Bearer " + config.api_key,
    });
  }
  append_extra_headers(headers, config);
  return headers;
}

} // namespace

// Config is immutable per Harness and validated once at Harness::create, so
// this adapter encodes the request without re-checking endpoint, auth, or
// sampling bounds.
Result<TransportRequest>
OpenAiAdapter::make_request(const Config& config, const ModelRequest& request) const {
  auto encoded = make_request_body(config, request);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return TransportRequest{
      .url = endpoint(config.base_url),
      .headers = request_headers(config),
      .body = std::move(*encoded),
      .provider_namespace = "openai",
      .tls_verify_peer = config.tls_verify_peer,
      .ca_bundle_path = config.ca_bundle_path,
      .proxy = config.proxy,
      .timeouts = config.timeouts,
      .limits = config.limits,
  };
}

} // namespace scry::detail
