#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/anthropic.hpp"
#include "provider/shared.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {

// The Messages API request as wire structs; see the note in provider/shared.hpp
// for why they are ordered, named, and placed the way they are.
struct AnthropicText {
  std::string_view text{};
  std::string_view type{"text"};
};

struct AnthropicToolUse {
  std::string_view id{};
  JsonText input{};
  std::string_view name{};
  std::string_view type{"tool_use"};
};

// `content` is a JSON string holding the result document, not the document
// itself, which is why it is a string_view and `input` above is JsonText.
struct AnthropicToolResult {
  std::string_view content{};
  bool is_error{};
  std::string_view tool_use_id{};
  std::string_view type{"tool_result"};
};

using AnthropicBlock =
    std::variant<AnthropicText, AnthropicToolUse, AnthropicToolResult>;

struct AnthropicMessage {
  std::vector<AnthropicBlock> content{};
  std::string_view role{};
};

struct AnthropicTool {
  std::string_view description{};
  JsonText input_schema{};
  std::string_view name{};
};

struct AnthropicBody {
  std::optional<std::uint32_t> max_tokens{};
  std::vector<AnthropicMessage> messages{};
  std::string_view model{};
  bool stream{true};
  std::optional<std::string_view> system{};
  double temperature{};
  std::optional<std::vector<AnthropicTool>> tools{};
  std::optional<double> top_p{};
};

namespace {

[[nodiscard]] Result<AnthropicBlock> encode_tool_call(const ToolCallBlock& block) {
  if (auto status = embedded_json_object(block.arguments.text,
                                         "Tool input must be a JSON object");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return AnthropicToolUse{
      .id = block.id,
      .input = JsonText{block.arguments.text},
      .name = block.name,
  };
}

[[nodiscard]] Result<AnthropicBlock> encode_tool_result(const ToolResultBlock& block) {
  if (auto status =
          embedded_json_value(block.result.text, "Tool result must be valid JSON");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return AnthropicToolResult{
      .content = block.result.text,
      .is_error = block.is_error,
      .tool_use_id = block.tool_call_id,
  };
}

[[nodiscard]] Result<AnthropicBlock> encode_content(const ContentBlock& block) {
  if (const auto* text = std::get_if<TextBlock>(&block)) {
    return AnthropicText{.text = text->text};
  }
  if (const auto* call = std::get_if<ToolCallBlock>(&block)) {
    return encode_tool_call(*call);
  }
  return encode_tool_result(std::get<ToolResultBlock>(block));
}

// The Messages API takes one message per role turn, so consecutive same-role
// messages are concatenated into a single content array rather than emitted as
// separate entries. A turn stopped at the tool-round limit commits history
// ending in the user message carrying that round's tool results, and the next
// send appends another user message straight after it.
[[nodiscard]] Status append_message(std::vector<AnthropicMessage>& encoded,
                                    const Message& message) {
  const std::string_view role = message.role == Role::user ? "user" : "assistant";
  if (encoded.empty() || encoded.back().role != role) {
    encoded.push_back(AnthropicMessage{.role = role});
  }
  auto& content = encoded.back().content;
  for (const auto& block : message.content) {
    auto wire = encode_content(block);
    if (!wire) {
      return std::unexpected(std::move(wire.error()));
    }
    // A wire block borrows every byte it carries, so it is trivially copyable
    // and there is nothing for a move to steal.
    content.push_back(*wire);
  }
  return {};
}

[[nodiscard]] Result<std::vector<AnthropicMessage>>
encode_messages(const ModelRequest& request) {
  std::vector<AnthropicMessage> encoded{};
  encoded.reserve(request.message_count());
  if (auto status = for_each_request_message(request,
                                             [&encoded](const Message& message) {
                                               return append_message(encoded, message);
                                             });
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return encoded;
}

[[nodiscard]] Result<std::optional<std::vector<AnthropicTool>>>
encode_tools(const ModelRequest& request) {
  if (!request.tools || request.tools->empty()) {
    return std::nullopt;
  }
  std::vector<AnthropicTool> encoded{};
  encoded.reserve(request.tools->size());
  for (const auto& tool : *request.tools) {
    if (auto status = embedded_json_object(tool.input_schema.text,
                                           "Tool input schema must be a JSON object");
        !status) {
      return std::unexpected(std::move(status.error()));
    }
    encoded.push_back(AnthropicTool{
        .description = tool.description,
        .input_schema = JsonText{tool.input_schema.text},
        .name = tool.name,
    });
  }
  return encoded;
}

[[nodiscard]] std::string endpoint(const std::string& configured) {
  auto base_url = trim_trailing_slashes(configured);
  constexpr auto path = std::string_view{"/v1/messages"};
  if (!base_url.ends_with(path)) {
    base_url.append(path);
  }
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

  // Validation rejects an unset max_tokens for this dialect, so the optional is
  // always engaged here; carrying it through keeps the encoder from reading an
  // empty one when a request is assembled by hand.
  const AnthropicBody body{
      .max_tokens = request.sampling.max_tokens,
      .messages = std::move(*messages),
      .model = config.model,
      .system = request.system_prompt.empty()
                    ? std::nullopt
                    : std::optional<std::string_view>{request.system_prompt},
      .temperature = request.sampling.temperature,
      .tools = std::move(*tools),
      .top_p = request.sampling.top_p,
  };
  return write_wire_json(body, ErrorCategory::invalid_config,
                         "Anthropic request body could not be encoded");
}

} // namespace

Result<TransportRequest>
AnthropicAdapter::make_request(const Config& config,
                               const ModelRequest& request) const {
  auto encoded = make_request_body(config, request);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return transport_request(
      config, endpoint(config.base_url),
      {
          HttpHeader{.name = "content-type", .value = "application/json"},
          HttpHeader{.name = "x-api-key", .value = config.api_key},
          HttpHeader{.name = "anthropic-version", .value = "2023-06-01"},
          HttpHeader{.name = "accept", .value = "text/event-stream"},
      },
      std::move(*encoded), "anthropic");
}

} // namespace scry::detail
