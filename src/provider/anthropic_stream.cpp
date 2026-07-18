#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/anthropic_error.hpp"
#include "provider/wire_json.hpp"

#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace scry::detail {
namespace {

[[nodiscard]] bool known_event(const std::string_view name) noexcept {
  return name == "message_start" || name == "content_block_start" ||
         name == "content_block_delta" || name == "content_block_stop" ||
         name == "message_delta" || name == "message_stop" || name == "error" ||
         name == "ping";
}

[[nodiscard]] Result<std::string_view> event_type(const std::string_view event_name,
                                                  const WireValue& root) {
  auto type = required_wire_string(root, "type");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (event_name != "message" && event_name != *type) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "Anthropic SSE event name and payload type do not match"));
  }
  return *type;
}

[[nodiscard]] Result<std::size_t> content_index(const WireValue& root) {
  auto index = optional_wire_uint(root, "index");
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (!*index ||
      **index > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "Anthropic content event has no usable block index"));
  }
  return static_cast<std::size_t>(**index);
}

[[nodiscard]] Result<const WireValue*> required_object(const WireValue& root,
                                                       const std::string_view name) {
  const auto* value = wire_field(root, name);
  if (value == nullptr || !value->is_object()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "Anthropic payload field '" + std::string{name} + "' must be an object"));
  }
  return value;
}

[[nodiscard]] std::string request_identifier(const WireValue& root) {
  auto value = optional_wire_string(root, "request_id");
  if (value && *value) {
    return std::string{**value};
  }
  return {};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
decode_initial_content(const WireValue& message, ProviderDecodeState& state) {
  auto values = required_wire_array(message, "content");
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }

  std::vector<ProviderEvent> events{};
  for (const auto& value : **values) {
    auto block = decode_anthropic_content(value, false);
    if (!block) {
      return std::unexpected(std::move(block.error()));
    }
    if (const auto* text = std::get_if<TextBlock>(&*block);
        text != nullptr && !text->text.empty()) {
      events.push_back(ProviderTextDelta{.text = text->text});
    }
    state.response.content.push_back(std::move(*block));
    state.semantic_output_consumed = true;
  }
  return events;
}

[[nodiscard]] Status apply_initial_finish(const WireValue& message,
                                          ProviderDecodeState& state) {
  auto reason = optional_wire_string(message, "stop_reason");
  if (!reason) {
    return std::unexpected(std::move(reason.error()));
  }
  auto finish = decode_anthropic_finish(*reason);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  state.response.finish_reason = *finish;
  state.finish_observed = reason->has_value();
  return {};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_message_start(const WireValue& root, ProviderDecodeState& state) {
  if (state.message_started) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "Anthropic stream emitted more than one message_start"));
  }
  auto message = required_object(root, "message");
  if (!message) {
    return std::unexpected(std::move(message.error()));
  }
  auto type = required_wire_string(**message, "type");
  if (!type || *type != "message") {
    return std::unexpected(
        type ? make_provider_error(ErrorCategory::protocol,
                                   "Anthropic message_start is not a message")
             : std::move(type.error()));
  }
  if (state.response.provider_request_id.empty()) {
    state.response.provider_request_id = request_identifier(**message);
  }
  auto usage = apply_anthropic_usage(**message, state.response.usage);
  if (!usage) {
    return std::unexpected(std::move(usage.error()));
  }
  auto finish = apply_initial_finish(**message, state);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  state.message_started = true;
  return decode_initial_content(**message, state);
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_content_start(const WireValue& root, ProviderDecodeState& state) {
  if (!state.message_started || state.active_content_index || state.finish_observed) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "Anthropic content block began outside the message lifecycle"));
  }
  auto index = content_index(root);
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (*index != state.response.content.size()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "Anthropic content blocks did not begin in contiguous order"));
  }
  auto value = required_object(root, "content_block");
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  auto block = decode_anthropic_content(**value, true);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }

  state.semantic_output_consumed = true;
  std::vector<ProviderEvent> events{};
  if (const auto* text = std::get_if<TextBlock>(&*block);
      text != nullptr && !text->text.empty()) {
    events.push_back(ProviderTextDelta{.text = text->text});
  }
  state.response.content.push_back(std::move(*block));
  state.active_content_index = *index;
  return events;
}

[[nodiscard]] Result<ContentBlock*> indexed_block(const WireValue& root,
                                                  ProviderDecodeState& state) {
  auto index = content_index(root);
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (*index >= state.response.content.size()) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "Anthropic content event referenced an unknown block"));
  }
  if (state.active_content_index != *index) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "Anthropic content event targeted a block that is not active"));
  }
  return &state.response.content[*index];
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_text_delta(const WireValue& delta, ContentBlock& block,
                  ProviderDecodeState& state) {
  auto text = required_wire_string(delta, "text");
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  auto* destination = std::get_if<TextBlock>(&block);
  if (destination == nullptr) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "Anthropic text delta targeted a non-text block"));
  }
  destination->text.append(*text);
  state.semantic_output_consumed = true;
  return std::vector<ProviderEvent>{ProviderTextDelta{
      .text = std::string{*text},
  }};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_json_delta(const WireValue& delta, ContentBlock& block,
                  ProviderDecodeState& state) {
  auto partial = required_wire_string(delta, "partial_json");
  if (!partial) {
    return std::unexpected(std::move(partial.error()));
  }
  auto* destination = std::get_if<ToolCallBlock>(&block);
  if (destination == nullptr) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "Anthropic input JSON delta targeted a non-tool block"));
  }
  destination->arguments.text.append(*partial);
  state.semantic_output_consumed = true;
  return std::vector<ProviderEvent>{};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_content_delta(const WireValue& root, ProviderDecodeState& state) {
  auto block = indexed_block(root, state);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }
  auto delta = required_object(root, "delta");
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto type = required_wire_string(**delta, "type");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type == "text_delta") {
    return handle_text_delta(**delta, **block, state);
  }
  if (*type == "input_json_delta") {
    return handle_json_delta(**delta, **block, state);
  }
  return std::unexpected(
      make_provider_error(ErrorCategory::protocol,
                          "Anthropic returned an unsupported required content delta"));
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_content_stop(const WireValue& root, ProviderDecodeState& state) {
  auto block = indexed_block(root, state);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }
  auto* tool = std::get_if<ToolCallBlock>(*block);
  if (tool == nullptr) {
    state.active_content_index.reset();
    return std::vector<ProviderEvent>{};
  }
  if (tool->arguments.text.empty()) {
    tool->arguments.text = "{}";
  }
  auto parsed = parse_wire_json(tool->arguments.text, ErrorCategory::protocol,
                                "Anthropic streamed tool input is not valid JSON");
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  auto canonical =
      write_wire_json(*parsed, ErrorCategory::protocol,
                      "Anthropic streamed tool input could not be preserved");
  if (!canonical) {
    return std::unexpected(std::move(canonical.error()));
  }
  tool->arguments.text = std::move(*canonical);
  state.active_content_index.reset();
  return std::vector<ProviderEvent>{};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_message_delta(const WireValue& root, ProviderDecodeState& state) {
  if (!state.message_started || state.active_content_index || state.finish_observed) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "Anthropic message_delta violated the message lifecycle"));
  }
  auto delta = required_object(root, "delta");
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto reason = optional_wire_string(**delta, "stop_reason");
  if (!reason) {
    return std::unexpected(std::move(reason.error()));
  }
  auto finish = decode_anthropic_finish(*reason);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  state.response.finish_reason = *finish;
  state.finish_observed = reason->has_value();
  auto usage = apply_anthropic_usage(root, state.response.usage);
  if (!usage) {
    return std::unexpected(std::move(usage.error()));
  }
  return std::vector<ProviderEvent>{};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
handle_message_stop(ProviderDecodeState& state) {
  if (!state.message_started || state.active_content_index || !state.finish_observed ||
      state.completed) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "Anthropic message_stop violated the message lifecycle"));
  }
  state.completed = true;
  return std::vector<ProviderEvent>{
      ProviderCompleted{.response = state.response},
  };
}

[[nodiscard]] Error stream_error(const WireValue& root) {
  auto type = std::string{"unknown_error"};
  if (const auto* value = wire_field(root, "error");
      value != nullptr && value->is_object()) {
    auto parsed = optional_wire_string(*value, "type");
    if (parsed && *parsed) {
      type = sanitize_anthropic_error_type(**parsed);
    }
  }

  ErrorCategory category = ErrorCategory::protocol;
  bool retryable = false;
  if (type == "authentication_error" || type == "permission_error") {
    category = ErrorCategory::authentication;
  } else if (type == "rate_limit_error") {
    category = ErrorCategory::rate_limit;
    retryable = true;
  } else if (type == "overloaded_error" || type == "api_error") {
    category = ErrorCategory::network;
    retryable = true;
  }
  Error error =
      make_provider_error(category, "Anthropic stream returned an error", retryable);
  error.provider_detail = "anthropic:" + type;
  error.provider_request_id = request_identifier(root);
  return error;
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
dispatch_event(const std::string_view type, const WireValue& root,
               ProviderDecodeState& state) {
  if (type == "message_start") {
    return handle_message_start(root, state);
  }
  if (type == "content_block_start") {
    return handle_content_start(root, state);
  }
  if (type == "content_block_delta") {
    return handle_content_delta(root, state);
  }
  if (type == "content_block_stop") {
    return handle_content_stop(root, state);
  }
  if (type == "message_delta") {
    return handle_message_delta(root, state);
  }
  if (type == "message_stop") {
    return handle_message_stop(state);
  }
  if (type == "error") {
    return std::unexpected(stream_error(root));
  }
  return std::vector<ProviderEvent>{ProviderIgnoredEvent{
      .name = std::string{type},
  }};
}

} // namespace

// The adjacent string views are fixed by the ProviderAdapter seam.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Result<std::vector<ProviderEvent>>
AnthropicAdapter::parse_stream_event(const std::string_view event_name,
                                     const std::string_view data,
                                     ProviderDecodeState& state) const {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (state.completed) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "Anthropic stream emitted data after its terminal event"));
  }
  if (event_name != "message" && !known_event(event_name)) {
    return std::vector<ProviderEvent>{ProviderIgnoredEvent{
        .name = std::string{event_name},
    }};
  }

  auto root = parse_wire_json(data, ErrorCategory::protocol,
                              "Anthropic SSE data is not valid JSON");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  auto type = event_type(event_name, *root);
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (!known_event(*type)) {
    return std::vector<ProviderEvent>{ProviderIgnoredEvent{
        .name = std::string{*type},
    }};
  }
  return dispatch_event(*type, *root, state);
}

} // namespace scry::detail
