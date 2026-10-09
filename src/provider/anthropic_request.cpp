#include "kernel/error.hpp"
#include "kernel/json/codec.hpp"
#include "provider/anthropic.hpp"
#include "provider/shared.hpp"

#include <cstdint>
#include <optional>
#include <scry/annotations.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

// The Messages API request as reflected wire structs; see the note in
// provider/shared.hpp for what they borrow.
struct[[= reflection::tag{"text"}]] AnthropicText {
  std::string_view text;
};

struct[[= reflection::tag{"tool_use"}]] AnthropicToolUse {
  std::string_view id;
  std::string_view name;
  const Json& input;
};

// `content` is a JSON string holding the result document, not the document
// itself, which is why it is a string_view and `input` above is a Json.
struct[[= reflection::tag{"tool_result"}]] AnthropicToolResult {
  std::string_view tool_use_id;
  std::string_view content;
  bool is_error;
};

using AnthropicBlock =
    std::variant<AnthropicText, AnthropicToolUse, AnthropicToolResult>;

struct AnthropicMessage {
  std::string_view role;
  std::vector<AnthropicBlock> content;
};

struct AnthropicTool {
  std::string_view name;
  std::string_view description;
  const Json& input_schema;
};

// `{"type":"any"}` requires the model to call one of the offered tools; a typed
// turn sends it so the model answers through its response tool.
struct AnthropicToolChoice {
  std::string_view type;
};

struct[[= reflection::skip_null]] AnthropicBody {
  std::string_view model;
  std::optional<std::uint32_t> max_tokens;
  std::optional<std::string_view> system;
  std::vector<AnthropicMessage> messages;
  std::optional<std::vector<AnthropicTool>> tools;
  std::optional<AnthropicToolChoice> tool_choice;
  Json temperature;
  std::optional<Json> top_p;
  bool stream;
};

[[nodiscard]] Result<AnthropicBlock> encode_tool_call(const ToolCallBlock& block) {
  if (auto status = embedded_object_root(block.arguments.text,
                                         "Tool input must be a JSON object");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return AnthropicToolUse{.id = block.id, .name = block.name, .input = block.arguments};
}

[[nodiscard]] Result<AnthropicBlock> encode_tool_result(const ToolResultBlock& block) {
  if (auto status =
          embedded_json_value(block.result.text, "Tool result must be valid JSON");
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return AnthropicToolResult{
      .tool_use_id = block.tool_call_id,
      .content = block.result.text,
      .is_error = block.is_error,
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
// separate entries. A calls-only response that stops a turn at the tool-round
// limit commits history ending in the user message carrying the last round's tool
// results, and the next send appends another user message straight after it.
[[nodiscard]] Status append_message(std::vector<AnthropicMessage>& encoded,
                                    const Message& message) {
  const std::string_view role = message.role == Role::user ? "user" : "assistant";
  if (encoded.empty() || encoded.back().role != role) {
    encoded.push_back(AnthropicMessage{.role = role, .content = {}});
  }
  auto& content = encoded.back().content;
  for (const auto& block : message.content) {
    auto wire = encode_content(block);
    if (!wire) {
      return std::unexpected(std::move(wire.error()));
    }
    content.push_back(std::move(*wire));
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

[[nodiscard]] Status append_tool(std::vector<AnthropicTool>& encoded,
                                 const ToolDefinition& tool) {
  if (auto status = embedded_object_root(tool.input_schema.text,
                                         "Tool input schema must be a JSON object");
      !status) {
    return status;
  }
  encoded.push_back(AnthropicTool{
      .name = tool.name,
      .description = tool.description,
      .input_schema = tool.input_schema,
  });
  return {};
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
  auto tools = encode_tools<AnthropicTool>(request, append_tool);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }

  // Validation rejects an unset max_tokens for this dialect, so the optional is
  // always engaged here; carrying it through keeps the encoder from reading an
  // empty one when a request is assembled by hand.
  const auto& sampling = request.sampling;
  const AnthropicBody body{
      .model = config.model,
      .max_tokens = sampling.max_tokens,
      .system = request.system_prompt.empty()
                    ? std::nullopt
                    : std::optional<std::string_view>{request.system_prompt},
      .messages = std::move(*messages),
      .tools = std::move(*tools),
      .tool_choice = request.response_tool
                         ? std::optional<AnthropicToolChoice>{{.type = "any"}}
                         : std::nullopt,
      .temperature = canonical_json_number(sampling.temperature),
      .top_p = sampling.top_p.transform(canonical_json_number),
      .stream = true,
  };
  return encode_request_body(body, "Anthropic");
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
