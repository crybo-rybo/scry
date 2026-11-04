#pragma once

#include "core/model.hpp"
#include "core/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <scrydantic/annotations.hpp>
#include <scrydantic/json.hpp>
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

struct[[ = scrydantic::tag{"text"}, = scrydantic::ignore_unknown ]]
    AnthropicTextContent {
  std::string text;
};

// `input` captures the canonical text of whatever value arrived; that it is an
// object is checked when the block is built.
struct[[ = scrydantic::tag{"tool_use"}, = scrydantic::ignore_unknown ]]
    AnthropicToolUseContent {
  std::string id;
  std::string name;
  scrydantic::Json input;
};

// Scry reads thinking blocks only to skip them; see anthropic_content_block.
struct[[ = scrydantic::tag{"thinking"}, = scrydantic::ignore_unknown ]]
    AnthropicThinkingContent {
  std::string thinking;
};

struct[[ = scrydantic::tag{"redacted_thinking"}, = scrydantic::ignore_unknown ]]
    AnthropicRedactedThinkingContent {
  std::string data;
};

using AnthropicContent =
    std::variant<AnthropicTextContent, AnthropicToolUseContent,
                 AnthropicThinkingContent, AnthropicRedactedThinkingContent>;

// Only non-negative integers that fit 64 bits are counts; null reads as absent.
struct[[ = scrydantic::ignore_unknown, = scrydantic::skip_null ]] AnthropicUsage {
  std::optional<std::uint64_t> input_tokens;
  std::optional<std::uint64_t> output_tokens;
};

// message_start's message. An absent `usage` is the empty default, while a null
// one is rejected, as it must be an object when present.
struct[[ = scrydantic::ignore_unknown, = scrydantic::skip_null ]]
    AnthropicResponseMessage {
  std::string type;
  std::vector<AnthropicContent> content;
  std::optional<std::string> stop_reason;
  AnthropicUsage usage{};
};

struct[[ = scrydantic::ignore_unknown, = scrydantic::skip_null ]]
    AnthropicMessageDeltaBody {
  std::optional<std::string> stop_reason;
};

struct[[ = scrydantic::tag{"text_delta"}, = scrydantic::ignore_unknown ]]
    AnthropicTextDelta {
  std::string text;
};

struct[[ = scrydantic::tag{"input_json_delta"}, = scrydantic::ignore_unknown ]]
    AnthropicInputJsonDelta {
  std::string partial_json;
};

struct[[ = scrydantic::tag{"thinking_delta"}, = scrydantic::ignore_unknown ]]
    AnthropicThinkingDelta {
  std::string thinking;
};

struct[[ = scrydantic::tag{"signature_delta"}, = scrydantic::ignore_unknown ]]
    AnthropicSignatureDelta {
  std::string signature;
};

using AnthropicDelta = std::variant<AnthropicTextDelta, AnthropicInputJsonDelta,
                                    AnthropicThinkingDelta, AnthropicSignatureDelta>;

// The stream's events, selected by their `type`. `request_id` is read apart from
// these, best-effort, because a malformed one must not fail its event.
struct[[ = scrydantic::tag{"message_start"}, = scrydantic::ignore_unknown ]]
    AnthropicMessageStart {
  AnthropicResponseMessage message;
};

struct[[ = scrydantic::tag{"content_block_start"}, = scrydantic::ignore_unknown ]]
    AnthropicContentBlockStart {
  std::size_t index;
  AnthropicContent content_block;
};

struct[[ = scrydantic::tag{"content_block_delta"}, = scrydantic::ignore_unknown ]]
    AnthropicContentBlockDelta {
  std::size_t index;
  AnthropicDelta delta;
};

struct[[ = scrydantic::tag{"content_block_stop"}, = scrydantic::ignore_unknown ]]
    AnthropicContentBlockStop {
  std::size_t index;
};

struct[[ = scrydantic::tag{"message_delta"}, = scrydantic::ignore_unknown ]]
    AnthropicMessageDelta {
  AnthropicMessageDeltaBody delta;
  AnthropicUsage usage{};
};

struct[[ = scrydantic::tag{"message_stop"}, = scrydantic::ignore_unknown ]]
    AnthropicMessageStop {};

// The error body is read best-effort, apart from this event; see stream_error.
struct[[ = scrydantic::tag{"error"}, = scrydantic::ignore_unknown ]]
    AnthropicErrorEvent {};

struct[[ = scrydantic::tag{"ping"}, = scrydantic::ignore_unknown ]] AnthropicPing {};

using AnthropicEvent =
    std::variant<AnthropicMessageStart, AnthropicContentBlockStart,
                 AnthropicContentBlockDelta, AnthropicContentBlockStop,
                 AnthropicMessageDelta, AnthropicMessageStop, AnthropicErrorEvent,
                 AnthropicPing>;

// Builds a response block. A streamed tool_use starts with empty arguments, which
// its input_json_delta events fill; one inside message_start carries its input.
// A thinking block builds no response block, so Scry never sends it back.
[[nodiscard]] Result<std::optional<ContentBlock>>
anthropic_content_block(AnthropicContent content, bool streaming_start);

// Which thinking block `content` is, if Scry skips it.
[[nodiscard]] AnthropicSkippedBlock
anthropic_skipped_block(const AnthropicContent& content) noexcept;

[[nodiscard]] FinishReason
decode_anthropic_finish(std::optional<std::string_view> reason);

void apply_anthropic_usage(const AnthropicUsage& reported, Usage& usage);

} // namespace scry::detail
