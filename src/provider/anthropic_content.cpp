#include "provider/anthropic_content.hpp"

#include <limits>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Result<ContentBlock> decode_text(const WireValue& value) {
  auto text = required_wire_string(value, "text");
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  return ContentBlock{TextBlock{.text = std::string{*text}}};
}

[[nodiscard]] Result<ContentBlock> decode_tool_call(const WireValue& value,
                                                    const bool streaming_start) {
  auto id = required_wire_string(value, "id");
  if (!id) {
    return std::unexpected(std::move(id.error()));
  }
  auto name = required_wire_string(value, "name");
  if (!name) {
    return std::unexpected(std::move(name.error()));
  }

  std::string arguments{};
  if (streaming_start) {
    arguments.clear();
  } else if (const auto* input = wire_field(value, "input"); input != nullptr) {
    auto encoded = write_wire_json(*input, ErrorCategory::protocol,
                                   "Anthropic tool input could not be preserved");
    if (!encoded) {
      return std::unexpected(std::move(encoded.error()));
    }
    arguments = std::move(*encoded);
  } else {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "Anthropic tool_use block is missing input"));
  }

  return ContentBlock{ToolCallBlock{
      .id = std::string{*id},
      .name = std::string{*name},
      .arguments = Json{.text = std::move(arguments)},
  }};
}

[[nodiscard]] Status assign_usage_count(const WireValue& usage,
                                        const std::string_view field,
                                        std::uint64_t& destination) {
  auto count = optional_wire_uint(usage, field);
  if (!count) {
    return std::unexpected(std::move(count.error()));
  }
  const auto parsed_count = *count;
  if (parsed_count) {
    destination = *parsed_count;
  }
  return {};
}

} // namespace

Result<ContentBlock> decode_anthropic_content(const WireValue& value,
                                              const bool streaming_start) {
  auto type = required_wire_string(value, "type");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type == "text") {
    return decode_text(value);
  }
  if (*type == "tool_use") {
    return decode_tool_call(value, streaming_start);
  }
  return std::unexpected(
      make_provider_error(ErrorCategory::protocol,
                          "Anthropic returned an unsupported required content block"));
}

Result<FinishReason>
decode_anthropic_finish(const std::optional<std::string_view> reason) {
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

Status apply_anthropic_usage(const WireValue& owner, Usage& usage) {
  const auto* value = wire_field(owner, "usage");
  if (value == nullptr) {
    return {};
  }
  if (!value->is_object()) {
    return std::unexpected(make_provider_error(ErrorCategory::protocol,
                                               "Anthropic usage must be an object"));
  }

  auto input = assign_usage_count(*value, "input_tokens", usage.input_tokens);
  if (!input) {
    return input;
  }
  return assign_usage_count(*value, "output_tokens", usage.output_tokens);
}

} // namespace scry::detail
