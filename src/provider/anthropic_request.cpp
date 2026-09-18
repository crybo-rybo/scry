#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/anthropic.hpp"
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

// The Messages API request, described as plain aggregates instead of a JSON
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
// same-named wire structs here and in openai_request.cpp would have their key
// tables merged by the linker and each dialect would serialize with the
// other's keys.
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
class MessageArrayBuilder {
public:
  explicit MessageArrayBuilder(const std::size_t expected_messages) {
    encoded_.reserve(expected_messages);
  }

  [[nodiscard]] Status append(const Message& message) {
    if (role_ && *role_ != message.role) {
      flush();
    }
    role_ = message.role;
    for (const auto& block : message.content) {
      auto encoded = encode_content(block);
      if (!encoded) {
        return std::unexpected(std::move(encoded.error()));
      }
      // A wire block borrows every byte it carries, so it is trivially
      // copyable and there is nothing for a move to steal.
      content_.push_back(*encoded);
    }
    return {};
  }

  [[nodiscard]] std::vector<AnthropicMessage> take() {
    flush();
    return std::move(encoded_);
  }

private:
  void flush() {
    if (!role_) {
      return;
    }
    encoded_.push_back(AnthropicMessage{
        .content = std::move(content_),
        .role = *role_ == Role::user ? "user" : "assistant",
    });
    content_.clear();
    role_.reset();
  }

  std::vector<AnthropicMessage> encoded_{};
  std::vector<AnthropicBlock> content_{};
  std::optional<Role> role_{};
};

[[nodiscard]] Result<std::vector<AnthropicMessage>>
encode_messages(const ModelRequest& request) {
  MessageArrayBuilder builder{request.message_count()};
  if (request.history) {
    for (const auto& message : *request.history) {
      if (auto status = builder.append(message); !status) {
        return std::unexpected(std::move(status.error()));
      }
    }
  }
  for (const auto& message : request.messages) {
    if (auto status = builder.append(message); !status) {
      return std::unexpected(std::move(status.error()));
    }
  }
  return builder.take();
}

[[nodiscard]] Result<std::vector<AnthropicTool>>
encode_tools(const std::vector<ToolDefinition>& tools) {
  std::vector<AnthropicTool> encoded{};
  encoded.reserve(tools.size());
  for (const auto& tool : tools) {
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

[[nodiscard]] std::optional<std::string_view> optional_text(const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  return std::string_view{value};
}

[[nodiscard]] Result<std::string> make_request_body(const Config& config,
                                                    const ModelRequest& request) {
  auto messages = encode_messages(request);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  std::optional<std::vector<AnthropicTool>> tools{};
  if (request.tools && !request.tools->empty()) {
    auto encoded = encode_tools(*request.tools);
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    tools = std::move(*encoded);
  }

  // Validation rejects an unset max_tokens for this dialect, so the optional is
  // always engaged here; carrying it through keeps the encoder from reading an
  // empty one when a request is assembled by hand.
  const AnthropicBody body{
      .max_tokens = request.sampling.max_tokens,
      .messages = std::move(*messages),
      .model = config.model,
      .system = optional_text(request.system_prompt),
      .temperature = request.sampling.temperature,
      .tools = std::move(tools),
      .top_p = request.sampling.top_p,
  };
  return write_wire_json(body, ErrorCategory::invalid_config,
                         "Anthropic request body could not be encoded");
}

} // namespace

// Config is immutable per Harness and validated once at Harness::create, so
// this adapter encodes the request without re-checking endpoint, auth, or
// sampling bounds.
Result<TransportRequest>
AnthropicAdapter::make_request(const Config& config,
                               const ModelRequest& request) const {
  auto encoded = make_request_body(config, request);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }

  std::vector<HttpHeader> headers{
      HttpHeader{.name = "content-type", .value = "application/json"},
      HttpHeader{.name = "x-api-key", .value = config.api_key},
      HttpHeader{.name = "anthropic-version", .value = "2023-06-01"},
      HttpHeader{.name = "accept", .value = "text/event-stream"},
  };
  append_extra_headers(headers, config);
  return TransportRequest{
      .url = endpoint(config.base_url),
      .headers = std::move(headers),
      .body = std::move(*encoded),
      .provider_namespace = "anthropic",
      .tls_verify_peer = config.tls_verify_peer,
      .ca_bundle_path = config.ca_bundle_path,
      .proxy = config.proxy,
      .timeouts = config.timeouts,
      .limits = config.limits,
  };
}

} // namespace scry::detail
