#include "provider/anthropic_content.hpp"

#include "kernel/error.hpp"

#include <optional>
#include <utility>
#include <variant>

namespace scry::detail {
namespace {

using OptionalBlock = Result<std::optional<ContentBlock>>;

[[nodiscard]] OptionalBlock content_block(AnthropicTextContent content, bool) {
  return ContentBlock{TextBlock{.text = std::move(content.text)}};
}

[[nodiscard]] OptionalBlock content_block(AnthropicToolUseContent content,
                                          const bool streaming_start) {
  // The captured input is canonical text, so an object starts with its brace.
  if (!content.input.text.starts_with('{')) {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "Anthropic tool_use input must be a JSON object"));
  }
  return ContentBlock{ToolCallBlock{
      .id = std::move(content.id),
      .name = std::move(content.name),
      .arguments = streaming_start ? Json{} : std::move(content.input),
  }};
}

[[nodiscard]] OptionalBlock content_block(const AnthropicThinkingContent&, bool) {
  return std::nullopt;
}

[[nodiscard]] OptionalBlock content_block(const AnthropicRedactedThinkingContent&,
                                          bool) {
  return std::nullopt;
}

// Deliberately divergent from the OpenAI adapter: Anthropic reports usage
// incrementally, so a missing field preserves the previously observed count
// instead of zeroing it.
void assign_usage_count(const std::optional<std::uint64_t>& count,
                        std::uint64_t& destination) {
  if (count) {
    destination = *count;
  }
}

} // namespace

Result<std::optional<ContentBlock>>
anthropic_content_block(AnthropicContent content, const bool streaming_start) {
  return std::visit(
      [streaming_start](auto& value) {
        return content_block(std::move(value), streaming_start);
      },
      content);
}

AnthropicSkippedBlock
anthropic_skipped_block(const AnthropicContent& content) noexcept {
  if (std::holds_alternative<AnthropicThinkingContent>(content)) {
    return AnthropicSkippedBlock::thinking;
  }
  if (std::holds_alternative<AnthropicRedactedThinkingContent>(content)) {
    return AnthropicSkippedBlock::redacted_thinking;
  }
  return AnthropicSkippedBlock::none;
}

FinishReason decode_anthropic_finish(const std::optional<std::string_view> reason) {
  if (!reason) {
    return FinishReason::unknown;
  }
  if (*reason == "end_turn" || *reason == "stop_sequence") {
    return FinishReason::completed;
  }
  if (*reason == "max_tokens") {
    return FinishReason::length;
  }
  if (*reason == "tool_use") {
    return FinishReason::tool_use;
  }
  return FinishReason::unknown;
}

void apply_anthropic_usage(const AnthropicUsage& reported, Usage& usage) {
  assign_usage_count(reported.input_tokens, usage.input_tokens);
  assign_usage_count(reported.output_tokens, usage.output_tokens);
}

} // namespace scry::detail
