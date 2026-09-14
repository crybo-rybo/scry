#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <scry/json.hpp>
#include <string>
#include <string_view>

/// @file
/// Builders for the provider response bodies a ScriptedTransport replays.
///
/// These produce the exact server-sent-event bytes each dialect's decoder
/// accepts, so a downstream test can describe a turn by what the model said
/// rather than by transcribing wire frames. Every string they embed is encoded
/// as a JSON string literal, so assistant text with newlines, quotes, or tabs
/// and a pretty-printed tool-argument document all survive the round trip.
/// Every builder returns a complete response body; hand it to
/// ScriptedResponse::body_chunks whole, or split it at any byte to prove a
/// decoder reassembles frames it did not receive intact.
///
/// The error-body builders are the non-2xx counterpart: pair one with a
/// ScriptedResponse::status the provider would have returned with it, because
/// the scripted transport classifies that status the way a real HTTP response
/// is classified rather than decoding the body as a stream.
namespace scry::testing {

/// Encodes a fragment as a complete JSON string literal, quotes included.
///
/// Every builder routes its strings through this, so a value carrying control
/// characters or quotes stays valid JSON on the wire instead of producing a
/// body the real decoder rejects.
/// @param text Fragment to encode.
/// @return The fragment as a quoted, escaped JSON string.
[[nodiscard]] inline std::string json_string(const std::string_view text) {
  return escape_json_string(text);
}

/// One tool call announced by a scripted stream.
///
/// `arguments` is the tool input as plain JSON; the builders encode it into the
/// dialect's wire form, and an empty value announces the call without streaming
/// any input.
struct ToolUseBlock {
  /// Provider-assigned call identifier echoed back with the tool result.
  std::string_view id{};
  /// Registered tool name the call dispatches to.
  std::string_view name{};
  /// Tool input as plain JSON.
  std::string_view arguments{};
};

/// Builds the canonical five-event Anthropic text completion.
/// @param text Assistant text delivered as one content delta.
/// @param message_id Provider message identifier.
/// @param request_id Correlation identifier carried in message_start; omitted
/// when empty.
/// @param input_tokens Prompt tokens reported by message_start.
/// @param output_tokens Completion tokens reported by message_delta.
/// @return A complete Anthropic response body.
[[nodiscard]] inline std::string anthropic_text_stream(
    const std::string_view text, const std::string_view message_id = "msg_test",
    const std::string_view request_id = {}, const std::uint32_t input_tokens = 2,
    const std::uint32_t output_tokens = 2) {
  auto correlation = std::string{};
  if (!request_id.empty()) {
    correlation = R"(,"request_id":)" + json_string(request_id);
  }
  auto stream = std::string{"event: message_start\ndata: "};
  stream += R"({"type":"message_start","message":{"id":)";
  stream += json_string(message_id);
  stream += correlation;
  stream += R"(,"type":"message","role":"assistant","content":[],)";
  stream += R"("model":"test-model","stop_reason":null,"usage":{"input_tokens":)";
  stream += std::to_string(input_tokens);
  stream += R"(,"output_tokens":0}}})";
  stream += "\n\nevent: content_block_start\ndata: ";
  stream += R"({"type":"content_block_start","index":0,)";
  stream += R"("content_block":{"type":"text","text":""}})";
  stream += "\n\nevent: content_block_delta\ndata: ";
  stream += R"({"type":"content_block_delta","index":0,)";
  stream += R"("delta":{"type":"text_delta","text":)";
  stream += json_string(text);
  stream += R"(}})";
  stream += "\n\nevent: content_block_stop\ndata: ";
  stream += R"({"type":"content_block_stop","index":0})";
  stream += "\n\nevent: message_delta\ndata: ";
  stream += R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},)";
  stream += R"("usage":{"output_tokens":)";
  stream += std::to_string(output_tokens);
  stream += R"(}})";
  stream += "\n\nevent: message_stop\ndata: ";
  stream += R"({"type":"message_stop"})";
  stream += "\n\n";
  return stream;
}

/// Builds the three events that carry one Anthropic tool_use block.
/// @param index Content-block index the events address.
/// @param block Call the block announces.
/// @return The block's start, optional input delta, and stop events.
[[nodiscard]] inline std::string
anthropic_tool_block_events(const std::size_t index, const ToolUseBlock& block) {
  const auto position = std::to_string(index);
  auto events = std::string{"event: content_block_start\ndata: "};
  events += R"({"type":"content_block_start","index":)" + position;
  events += R"(,"content_block":{"type":"tool_use","id":)";
  events += json_string(block.id);
  events += R"(,"name":)";
  events += json_string(block.name);
  events += R"(,"input":{}}})";
  events += "\n\n";
  if (!block.arguments.empty()) {
    events += "event: content_block_delta\ndata: ";
    events += R"({"type":"content_block_delta","index":)" + position;
    events += R"(,"delta":{"type":"input_json_delta","partial_json":)";
    events += json_string(block.arguments);
    events += R"(}})";
    events += "\n\n";
  }
  events += "event: content_block_stop\ndata: ";
  events += R"({"type":"content_block_stop","index":)" + position + "}";
  events += "\n\n";
  return events;
}

/// Builds an Anthropic stream whose content is one or more tool_use blocks.
///
/// The stop reason is a parameter because a stream that announces tool calls
/// and then ends the turn anyway is itself a case worth scripting. Reported
/// output tokens are the block count.
/// @param blocks Calls the assistant announces, in content-block order.
/// @param message_id Provider message identifier.
/// @param stop_reason Anthropic stop reason reported by message_delta.
/// @param input_tokens Prompt tokens reported by message_start.
/// @return A complete Anthropic response body.
[[nodiscard]] inline std::string
anthropic_tool_stream(const std::initializer_list<ToolUseBlock> blocks,
                      const std::string_view message_id = "msg_tools",
                      const std::string_view stop_reason = "tool_use",
                      const std::uint32_t input_tokens = 3) {
  auto stream = std::string{"event: message_start\ndata: "};
  stream += R"({"type":"message_start","message":{"id":)";
  stream += json_string(message_id);
  stream += R"(,"type":"message","role":"assistant","content":[],)";
  stream += R"("model":"test-model","stop_reason":null,"usage":{"input_tokens":)";
  stream += std::to_string(input_tokens);
  stream += R"(,"output_tokens":0}}})";
  stream += "\n\n";
  auto index = std::size_t{0};
  for (const auto& block : blocks) {
    stream += anthropic_tool_block_events(index, block);
    ++index;
  }
  stream += "event: message_delta\ndata: ";
  stream += R"({"type":"message_delta","delta":{"stop_reason":)";
  stream += json_string(stop_reason);
  stream += R"(},"usage":{"output_tokens":)";
  stream += std::to_string(blocks.size());
  stream += R"(}})";
  stream += "\n\nevent: message_stop\ndata: ";
  stream += R"({"type":"message_stop"})";
  stream += "\n\n";
  return stream;
}

/// Builds the canonical OpenAI-compatible text completion.
///
/// A role chunk, one content delta, a finish chunk, a usage chunk, then the
/// terminating sentinel. An empty `text` still emits the content delta, because
/// a compatible server streaming nothing is itself a case worth scripting.
/// @param text Assistant text delivered as one content delta.
/// @param completion_id Chat-completion identifier repeated by every chunk.
/// @param prompt_tokens Prompt tokens reported by the usage chunk.
/// @param completion_tokens Completion tokens reported by the usage chunk.
/// @return A complete OpenAI-compatible response body.
[[nodiscard]] inline std::string openai_text_stream(
    const std::string_view text, const std::string_view completion_id = "chatcmpl-test",
    const std::uint32_t prompt_tokens = 4, const std::uint32_t completion_tokens = 2) {
  const auto prefix = R"(data: {"id":)" + json_string(completion_id) +
                      R"(,"object":"chat.completion.chunk","choices":)";
  auto stream = prefix;
  stream += R"([{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]})";
  stream += "\n\n";
  stream += prefix;
  stream += R"([{"index":0,"delta":{"content":)";
  stream += json_string(text);
  stream += R"(},"finish_reason":null}]})";
  stream += "\n\n";
  stream += prefix;
  stream += R"([{"index":0,"delta":{},"finish_reason":"stop"}]})";
  stream += "\n\n";
  stream += prefix;
  stream += R"([],"usage":{"prompt_tokens":)";
  stream += std::to_string(prompt_tokens);
  stream += R"(,"completion_tokens":)";
  stream += std::to_string(completion_tokens);
  stream += R"(,"total_tokens":)";
  stream += std::to_string(prompt_tokens + completion_tokens);
  stream += R"(}})";
  stream += "\n\ndata: [DONE]\n\n";
  return stream;
}

/// Builds one OpenAI-compatible tool_calls entry.
/// @param index Position of the call within the assistant's tool_calls array.
/// @param block Call the entry announces.
/// @return The entry as a JSON object.
[[nodiscard]] inline std::string openai_tool_call_entry(const std::size_t index,
                                                        const ToolUseBlock& block) {
  auto entry = std::string{R"({"index":)"} + std::to_string(index);
  entry += R"(,"id":)";
  entry += json_string(block.id);
  entry += R"(,"type":"function","function":{"name":)";
  entry += json_string(block.name);
  entry += R"(,"arguments":)";
  entry += json_string(block.arguments);
  entry += R"(}})";
  return entry;
}

/// Builds an OpenAI-compatible stream that announces one or more tool calls.
///
/// Every call arrives complete in the first chunk and the turn finishes with
/// `tool_calls`, which is the shape a compatible server produces once its
/// arguments have been fully generated. Splitting the returned body across
/// chunks still exercises the decoder's reassembly.
/// @param blocks Calls the assistant announces, in tool_calls order.
/// @param completion_id Chat-completion identifier repeated by every chunk.
/// @param prompt_tokens Prompt tokens reported by the usage chunk.
/// @param completion_tokens Completion tokens reported by the usage chunk.
/// @return A complete OpenAI-compatible response body.
[[nodiscard]] inline std::string
openai_tool_stream(const std::initializer_list<ToolUseBlock> blocks,
                   const std::string_view completion_id = "chatcmpl-tools",
                   const std::uint32_t prompt_tokens = 4,
                   const std::uint32_t completion_tokens = 3) {
  const auto prefix = R"(data: {"id":)" + json_string(completion_id) +
                      R"(,"object":"chat.completion.chunk","choices":)";
  auto stream = prefix;
  stream += R"([{"index":0,"delta":{"role":"assistant","tool_calls":[)";
  auto index = std::size_t{0};
  for (const auto& block : blocks) {
    if (index != 0) {
      stream += ",";
    }
    stream += openai_tool_call_entry(index, block);
    ++index;
  }
  stream += R"(]},"finish_reason":null}]})";
  stream += "\n\n";
  stream += prefix;
  stream += R"([{"index":0,"delta":{},"finish_reason":"tool_calls"}]})";
  stream += "\n\n";
  stream += prefix;
  stream += R"([],"usage":{"prompt_tokens":)";
  stream += std::to_string(prompt_tokens);
  stream += R"(,"completion_tokens":)";
  stream += std::to_string(completion_tokens);
  stream += R"(,"total_tokens":)";
  stream += std::to_string(prompt_tokens + completion_tokens);
  stream += R"(}})";
  stream += "\n\ndata: [DONE]\n\n";
  return stream;
}

/// Builds the JSON body an OpenAI-compatible server returns with a non-2xx
/// status.
///
/// This is a plain HTTP error body, not a stream: pair it with
/// `ScriptedResponse::status`, which decides the error's category and whether
/// the runtime retries. The body only supplies the sanitized
/// `openai:<type>` token the error carries as its provider_detail.
/// @param status HTTP status the server reported, echoed as the numeric code.
/// @param type Provider error type, for example `rate_limit_exceeded`.
/// @return A complete OpenAI-compatible error body.
[[nodiscard]] inline std::string openai_error_body(const int status,
                                                   const std::string_view type) {
  auto body = std::string{R"({"error":{"message":"scripted error","type":)"};
  body += json_string(type);
  body += R"(,"code":)";
  body += std::to_string(status);
  body += "}}";
  return body;
}

/// Builds the JSON body Anthropic returns with a non-2xx status.
///
/// The Anthropic counterpart of openai_error_body: pair it with
/// `ScriptedResponse::status`, which decides the error's category and whether
/// the runtime retries, while the body supplies the sanitized
/// `anthropic:<type>` provider_detail token.
/// @param type Provider error type, for example `rate_limit_error`.
/// @return A complete Anthropic error body.
[[nodiscard]] inline std::string anthropic_error_body(const std::string_view type) {
  auto body = std::string{R"({"type":"error","error":{"type":)"};
  body += json_string(type);
  body += R"(,"message":"scripted error"}})";
  return body;
}

} // namespace scry::testing
