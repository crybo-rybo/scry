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

// The Chat Completions request as wire structs; see the note in
// provider/shared.hpp for why they are ordered, named, and placed the way they
// are.

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

// A user message is either plain text or a run of tool results, each of which
// becomes its own `tool` message. The shape is checked before anything is
// appended.
[[nodiscard]] Status encode_user_message(std::vector<OpenAiMessage>& encoded,
                                         const Message& message) {
  bool saw_text = false;
  bool saw_result = false;
  for (const auto& block : message.content) {
    if (std::holds_alternative<TextBlock>(block)) {
      saw_text = true;
    } else if (std::holds_alternative<ToolResultBlock>(block)) {
      saw_result = true;
    } else {
      return std::unexpected(
          invalid_request("OpenAI user messages cannot contain tool calls"));
    }
  }
  if (saw_text && saw_result) {
    return std::unexpected(
        invalid_request("OpenAI user messages cannot mix text and tool results"));
  }
  if (!saw_result) {
    encoded.push_back(OpenAiMessage{.content = message_text(message), .role = "user"});
    return {};
  }
  for (const auto& block : message.content) {
    auto value = encode_tool_result(std::get<ToolResultBlock>(block));
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    encoded.push_back(std::move(*value));
  }
  return {};
}

[[nodiscard]] Status encode_assistant_message(std::vector<OpenAiMessage>& encoded,
                                              const Message& message) {
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
    auto wire = encode_tool_call(*call);
    if (!wire) {
      return std::unexpected(std::move(wire.error()));
    }
    // A wire tool call borrows every byte it carries, so it is trivially
    // copyable and there is nothing for a move to steal.
    calls.push_back(*wire);
  }

  OpenAiMessage value{.content = message_text(message), .role = "assistant"};
  if (!calls.empty()) {
    if (text_is_empty(value.content)) {
      value.content = std::monostate{};
    }
    value.tool_calls = std::move(calls);
  }
  encoded.push_back(std::move(value));
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
  if (auto status = for_each_request_message(
          request,
          [&encoded](const Message& message) {
            return message.role == Role::user
                       ? encode_user_message(encoded, message)
                       : encode_assistant_message(encoded, message);
          });
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return encoded;
}

[[nodiscard]] Result<std::optional<std::vector<OpenAiTool>>>
encode_tools(const ModelRequest& request) {
  if (!request.tools || request.tools->empty()) {
    return std::nullopt;
  }
  std::vector<OpenAiTool> encoded{};
  encoded.reserve(request.tools->size());
  for (const auto& tool : *request.tools) {
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

[[nodiscard]] std::string endpoint(const std::string& configured) {
  auto base_url = trim_trailing_slashes(configured);
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
  auto tools = encode_tools(request);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }

  const OpenAiBody body{
      .max_tokens = request.sampling.max_tokens,
      .messages = std::move(*messages),
      .model = config.model,
      .reasoning_effort = config.reasoning_mode == ReasoningMode::disabled
                              ? std::optional<std::string_view>{"none"}
                              : std::nullopt,
      .temperature = request.sampling.temperature,
      .tools = std::move(*tools),
      .top_p = request.sampling.top_p,
  };
  return write_wire_json(body, ErrorCategory::invalid_config,
                         "OpenAI request body could not be encoded");
}

} // namespace

Result<TransportRequest>
OpenAiAdapter::make_request(const Config& config, const ModelRequest& request) const {
  auto encoded = make_request_body(config, request);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
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
  return transport_request(config, endpoint(config.base_url), std::move(headers),
                           std::move(*encoded), "openai");
}

} // namespace scry::detail
