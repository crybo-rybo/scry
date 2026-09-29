#pragma once

#include "core/model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <scry/annotations.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Anthropic Messages stream payloads as reflected aggregates, decoded by the
// reflected codec. Every class ignores members it does not declare, because the
// API adds fields, and every skip_null class reads an absent or null optional
// member as disengaged. What the codec cannot express - event order, block
// indices, and the lifecycle - stays in anthropic_stream.cpp.
namespace scry::detail {

struct[
    [ = reflection::tag{"text"}, = reflection::ignore_unknown ]] AnthropicTextContent {
  std::string text;
};

// `input` captures the canonical text of whatever value arrived; that it is an
// object is checked when the block is built.
struct[[
  = reflection::tag{"tool_use"}, = reflection::ignore_unknown
]] AnthropicToolUseContent {
  std::string id;
  std::string name;
  Json input;
};

using AnthropicContent = std::variant<AnthropicTextContent, AnthropicToolUseContent>;

// Only non-negative integers that fit 64 bits are counts; null reads as absent.
struct[[ = reflection::ignore_unknown, = reflection::skip_null ]] AnthropicUsage {
  std::optional<std::uint64_t> input_tokens;
  std::optional<std::uint64_t> output_tokens;
};

// message_start's message. An absent `usage` is the empty default, while a null
// one is rejected, as it must be an object when present.
struct[[
  = reflection::ignore_unknown, = reflection::skip_null
]] AnthropicResponseMessage {
  std::string type;
  std::vector<AnthropicContent> content;
  std::optional<std::string> stop_reason;
  AnthropicUsage usage{};
};

struct[[
  = reflection::ignore_unknown, = reflection::skip_null
]] AnthropicMessageDeltaBody {
  std::optional<std::string> stop_reason;
};

struct[[
  = reflection::tag{"text_delta"}, = reflection::ignore_unknown
]] AnthropicTextDelta {
  std::string text;
};

struct[[
  = reflection::tag{"input_json_delta"}, = reflection::ignore_unknown
]] AnthropicInputJsonDelta {
  std::string partial_json;
};

using AnthropicDelta = std::variant<AnthropicTextDelta, AnthropicInputJsonDelta>;

// The stream's events, selected by their `type`. `request_id` is read apart from
// these, best-effort, because a malformed one must not fail its event.
struct[[
  = reflection::tag{"message_start"}, = reflection::ignore_unknown
]] AnthropicMessageStart {
  AnthropicResponseMessage message;
};

struct[[
  = reflection::tag{"content_block_start"}, = reflection::ignore_unknown
]] AnthropicContentBlockStart {
  std::size_t index;
  AnthropicContent content_block;
};

struct[[
  = reflection::tag{"content_block_delta"}, = reflection::ignore_unknown
]] AnthropicContentBlockDelta {
  std::size_t index;
  AnthropicDelta delta;
};

struct[[
  = reflection::tag{"content_block_stop"}, = reflection::ignore_unknown
]] AnthropicContentBlockStop {
  std::size_t index;
};

struct[[
  = reflection::tag{"message_delta"}, = reflection::ignore_unknown
]] AnthropicMessageDelta {
  AnthropicMessageDeltaBody delta;
  AnthropicUsage usage{};
};

struct[[
  = reflection::tag{"message_stop"}, = reflection::ignore_unknown
]] AnthropicMessageStop {};

// The error body is read best-effort, apart from this event; see stream_error.
struct[
    [ = reflection::tag{"error"}, = reflection::ignore_unknown ]] AnthropicErrorEvent {
};

struct[[ = reflection::tag{"ping"}, = reflection::ignore_unknown ]] AnthropicPing {};

using AnthropicEvent =
    std::variant<AnthropicMessageStart, AnthropicContentBlockStart,
                 AnthropicContentBlockDelta, AnthropicContentBlockStop,
                 AnthropicMessageDelta, AnthropicMessageStop, AnthropicErrorEvent,
                 AnthropicPing>;

// Builds a response block. A streamed tool_use starts with empty arguments, which
// its input_json_delta events fill; one inside message_start carries its input.
[[nodiscard]] Result<ContentBlock> anthropic_content_block(AnthropicContent content,
                                                           bool streaming_start);

[[nodiscard]] FinishReason
decode_anthropic_finish(std::optional<std::string_view> reason);

void apply_anthropic_usage(const AnthropicUsage& reported, Usage& usage);

} // namespace scry::detail
