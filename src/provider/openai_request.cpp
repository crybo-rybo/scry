#include "kernel/error.hpp"
#include "kernel/json/codec.hpp"
#include "provider/openai.hpp"
#include "provider/shared.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <scry/annotations.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

// The Chat Completions request as reflected wire structs; see the note in
// provider/shared.hpp for what they borrow.

// `arguments` is a JSON string holding the argument document, not the document
// itself, which is why it is a string_view and `parameters` below is a Json.
struct OpenAiCallFunction {
  std::string_view name;
  std::string_view arguments;
};

struct OpenAiToolCall {
  std::string_view id;
  std::string_view type;
  OpenAiCallFunction function;
};

// An assistant message with tool calls and no text sends `"content": null`, a
// distinct wire value from an omitted member, so `content` keeps its null while
// the class omits the others when they are disengaged.
struct[[= reflection::skip_null]] OpenAiMessage {
  std::string_view role;
  [[= reflection::emit_null]] std::optional<std::string_view> content;
  std::optional<std::string_view> tool_call_id;
  std::optional<std::vector<OpenAiToolCall>> tool_calls;
};

struct OpenAiToolFunction {
  std::string_view name;
  std::string_view description;
  const Json& parameters;
};

struct OpenAiTool {
  std::string_view type;
  OpenAiToolFunction function;
};

struct OpenAiStreamOptions {
  bool include_usage;
};

struct[[= reflection::skip_null]] OpenAiBody {
  std::string_view model;
  std::vector<OpenAiMessage> messages;
  std::optional<std::vector<OpenAiTool>> tools;
  // "required" makes the model call one of the offered tools; a typed turn sends
  // it so the model answers through its response tool.
  std::optional<std::string_view> tool_choice;
  Json temperature;
  std::optional<Json> top_p;
  std::optional<std::uint32_t> max_tokens;
  std::optional<std::uint32_t> seed;
  std::optional<std::string_view> reasoning_effort;
  bool stream;
  OpenAiStreamOptions stream_options;
};

// Text joined from several blocks, owned for the length of one encode. A deque
// never moves an element it already holds, so each view into it stays valid.
using JoinedText = std::deque<std::string>;

[[nodiscard]] Error invalid_request(std::string message) {
  return make_error(ErrorCategory::invalid_config, std::move(message));
}

// Concatenates the message's text blocks. The single-block case - what a turn
// actually commits - borrows the block's bytes; anything else is joined once
// into storage the encode owns.
[[nodiscard]] std::string_view message_text(const Message& message,
                                            JoinedText& joined) {
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
    return single->text;
  }
  auto& text = joined.emplace_back();
  text.reserve(bytes);
  for (const auto& block : message.content) {
    if (const auto* part = std::get_if<TextBlock>(&block)) {
      text.append(part->text);
    }
  }
  return text;
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
      .id = call.id,
      .type = "function",
      .function =
          OpenAiCallFunction{.name = call.name, .arguments = call.arguments.text},
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
      .role = "tool",
      .content = result.result.text,
      .tool_call_id = result.tool_call_id,
      .tool_calls = std::nullopt,
  };
}

// A user message is either plain text or a run of tool results, each of which
// becomes its own `tool` message. The shape is checked before anything is
// appended.
[[nodiscard]] Status encode_user_message(std::vector<OpenAiMessage>& encoded,
                                         JoinedText& joined, const Message& message) {
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
    encoded.push_back(OpenAiMessage{.role = "user",
                                    .content = message_text(message, joined),
                                    .tool_call_id = std::nullopt,
                                    .tool_calls = std::nullopt});
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
                                              JoinedText& joined,
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
    calls.push_back(*wire);
  }

  OpenAiMessage value{.role = "assistant",
                      .content = message_text(message, joined),
                      .tool_call_id = std::nullopt,
                      .tool_calls = std::nullopt};
  if (!calls.empty()) {
    if (value.content->empty()) {
      value.content.reset();
    }
    value.tool_calls = std::move(calls);
  }
  encoded.push_back(std::move(value));
  return {};
}

[[nodiscard]] Result<std::vector<OpenAiMessage>>
encode_messages(const ModelRequest& request, JoinedText& joined) {
  std::vector<OpenAiMessage> encoded{};
  encoded.reserve(request.message_count() + 1U);
  if (!request.system_prompt.empty()) {
    encoded.push_back(OpenAiMessage{.role = "system",
                                    .content = request.system_prompt,
                                    .tool_call_id = std::nullopt,
                                    .tool_calls = std::nullopt});
  }
  if (auto status = for_each_request_message(
          request,
          [&encoded, &joined](const Message& message) {
            return message.role == Role::user
                       ? encode_user_message(encoded, joined, message)
                       : encode_assistant_message(encoded, joined, message);
          });
      !status) {
    return std::unexpected(std::move(status.error()));
  }
  return encoded;
}

[[nodiscard]] Status append_tool(std::vector<OpenAiTool>& encoded,
                                 const ToolDefinition& tool) {
  if (tool.name.empty()) {
    return std::unexpected(invalid_request("OpenAI tools require a nonempty name"));
  }
  if (auto status = embedded_object_root(tool.input_schema.text,
                                         "OpenAI tool schema must be a JSON object");
      !status) {
    return status;
  }
  encoded.push_back(OpenAiTool{
      .type = "function",
      .function =
          OpenAiToolFunction{
              .name = tool.name,
              .description = tool.description,
              .parameters = tool.input_schema,
          },
  });
  return {};
}

// The registered tools, then a typed turn's response tool.
[[nodiscard]] Result<std::optional<std::vector<OpenAiTool>>>
encode_tools(const ModelRequest& request) {
  const auto registered = request.tools ? request.tools->size() : 0U;
  if (registered == 0 && !request.response_tool) {
    return std::nullopt;
  }
  std::vector<OpenAiTool> encoded{};
  encoded.reserve(registered + 1U);
  auto status = for_each_request_tool(request, [&encoded](const ToolDefinition& tool) {
    return append_tool(encoded, tool);
  });
  if (!status) {
    return std::unexpected(std::move(status.error()));
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
  JoinedText joined{};
  auto messages = encode_messages(request, joined);
  if (!messages) {
    return std::unexpected(std::move(messages.error()));
  }
  auto tools = encode_tools(request);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }

  const auto& sampling = request.sampling;
  const OpenAiBody body{
      .model = config.model,
      .messages = std::move(*messages),
      .tools = std::move(*tools),
      .tool_choice = request.response_tool ? std::optional<std::string_view>{"required"}
                                           : std::nullopt,
      .temperature = canonical_json_number(sampling.temperature),
      .top_p = sampling.top_p.transform(canonical_json_number),
      .max_tokens = sampling.max_tokens,
      .seed = sampling.seed,
      .reasoning_effort = config.reasoning_mode == ReasoningMode::disabled
                              ? std::optional<std::string_view>{"none"}
                              : std::nullopt,
      .stream = true,
      .stream_options = OpenAiStreamOptions{.include_usage = true},
  };
  return encode_request_body(body, "OpenAI");
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
