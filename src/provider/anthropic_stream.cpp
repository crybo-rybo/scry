#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/shared.hpp"

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

[[nodiscard]] Result<std::size_t> content_index(const JsonValue& root) {
  return required_index(root, "Anthropic content event has no usable block index");
}

[[nodiscard]] std::string request_identifier(const JsonValue& root) {
  const auto parsed = optional_json_string(root, "request_id");
  if (!parsed) {
    return {};
  }
  const auto id = *parsed;
  return id ? std::string{*id} : std::string{};
}

// Appends a decoded block to the response, publishing any text it starts with.
void push_block(ContentBlock block, ProviderDecodeState& state,
                std::vector<ProviderEvent>& out) {
  if (const auto* text = std::get_if<TextBlock>(&block);
      text != nullptr && !text->text.empty()) {
    out.push_back(ProviderTextDelta{.text = text->text});
  }
  state.response.content.push_back(std::move(block));
  state.semantic_output_consumed = true;
}

[[nodiscard]] Status decode_initial_content(const JsonValue& message,
                                            ProviderDecodeState& state,
                                            std::vector<ProviderEvent>& out) {
  auto values = required_json_array(message, "content");
  if (!values) {
    return std::unexpected(std::move(values.error()));
  }

  for (const auto& value : **values) {
    auto block = decode_anthropic_content(value, false);
    if (!block) {
      return std::unexpected(std::move(block.error()));
    }
    push_block(std::move(*block), state, out);
  }
  return {};
}

// Reads `owner.stop_reason`, present on both message_start's message and
// message_delta's delta; a non-null reason marks the message finished.
[[nodiscard]] Status apply_stop_reason(const JsonValue& owner,
                                       ProviderDecodeState& state,
                                       AnthropicProviderDecodeState& decode) {
  auto reason = optional_json_string(owner, "stop_reason");
  if (!reason) {
    return std::unexpected(std::move(reason.error()));
  }
  state.response.finish_reason = decode_anthropic_finish(*reason);
  decode.finish_observed = reason->has_value();
  return {};
}

[[nodiscard]] Status handle_message_start(const JsonValue& root,
                                          ProviderDecodeState& state,
                                          AnthropicProviderDecodeState& decode,
                                          std::vector<ProviderEvent>& out) {
  if (decode.message_started) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic stream emitted more than one message_start"));
  }
  auto message = required_json_object(root, "message");
  if (!message) {
    return std::unexpected(std::move(message.error()));
  }
  auto type = required_json_string(**message, "type");
  if (!type || *type != "message") {
    return std::unexpected(type ? make_error(ErrorCategory::protocol,
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
  auto finish = apply_stop_reason(**message, state, decode);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  decode.message_started = true;
  return decode_initial_content(**message, state, out);
}

[[nodiscard]] Status handle_content_start(const JsonValue& root,
                                          ProviderDecodeState& state,
                                          AnthropicProviderDecodeState& decode,
                                          std::vector<ProviderEvent>& out) {
  if (!decode.message_started || decode.active_content_index ||
      decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content block began outside the message lifecycle"));
  }
  auto index = content_index(root);
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (*index != state.response.content.size()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content blocks did not begin in contiguous order"));
  }
  auto value = required_json_object(root, "content_block");
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  auto block = decode_anthropic_content(**value, true);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }
  push_block(std::move(*block), state, out);
  decode.active_content_index = *index;
  return {};
}

[[nodiscard]] Result<ContentBlock*>
indexed_block(const JsonValue& root, ProviderDecodeState& state,
              AnthropicProviderDecodeState& decode) {
  auto index = content_index(root);
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (*index >= state.response.content.size()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content event referenced an unknown block"));
  }
  if (decode.active_content_index != *index) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content event targeted a block that is not active"));
  }
  return &state.response.content[*index];
}

[[nodiscard]] Status handle_text_delta(const JsonValue& delta, ContentBlock& block,
                                       ProviderDecodeState& state,
                                       std::vector<ProviderEvent>& out) {
  auto text = required_json_string(delta, "text");
  if (!text) {
    return std::unexpected(std::move(text.error()));
  }
  auto* destination = std::get_if<TextBlock>(&block);
  if (destination == nullptr) {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "Anthropic text delta targeted a non-text block"));
  }
  destination->text.append(*text);
  state.semantic_output_consumed = true;
  out.push_back(ProviderTextDelta{
      .text = std::string{*text},
  });
  return {};
}

[[nodiscard]] Status handle_json_delta(const JsonValue& delta, ContentBlock& block,
                                       ProviderDecodeState& state) {
  auto partial = required_json_string(delta, "partial_json");
  if (!partial) {
    return std::unexpected(std::move(partial.error()));
  }
  auto* destination = std::get_if<ToolCallBlock>(&block);
  if (destination == nullptr) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic input JSON delta targeted a non-tool block"));
  }
  if (auto status = append_tool_arguments(
          destination->arguments.text, *partial, state.max_tool_arguments_bytes,
          "Anthropic tool arguments exceed the configured byte limit");
      !status) {
    return status;
  }
  state.semantic_output_consumed = true;
  return {};
}

[[nodiscard]] Status handle_content_delta(const JsonValue& root,
                                          ProviderDecodeState& state,
                                          AnthropicProviderDecodeState& decode,
                                          std::vector<ProviderEvent>& out) {
  auto block = indexed_block(root, state, decode);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }
  auto delta = required_json_object(root, "delta");
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto type = required_json_string(**delta, "type");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type == "text_delta") {
    return handle_text_delta(**delta, **block, state, out);
  }
  if (*type == "input_json_delta") {
    return handle_json_delta(**delta, **block, state);
  }
  return std::unexpected(
      make_error(ErrorCategory::protocol,
                 "Anthropic returned an unsupported required content delta"));
}

[[nodiscard]] Status handle_content_stop(const JsonValue& root,
                                         ProviderDecodeState& state,
                                         AnthropicProviderDecodeState& decode) {
  auto block = indexed_block(root, state, decode);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }
  if (auto* tool = std::get_if<ToolCallBlock>(*block)) {
    if (tool->arguments.text.empty()) {
      tool->arguments.text = "{}";
    }
    // The stream layer only proves the arguments are a JSON object and forwards
    // the bytes as received; TurnMachine owns the single canonicalization pass.
    if (auto status =
            validate_json_object(tool->arguments.text, ErrorCategory::protocol,
                                 "Anthropic streamed tool input must be a JSON object");
        !status) {
      return status;
    }
  }
  decode.active_content_index.reset();
  return {};
}

[[nodiscard]] Status handle_message_delta(const JsonValue& root,
                                          ProviderDecodeState& state,
                                          AnthropicProviderDecodeState& decode) {
  if (!decode.message_started || decode.active_content_index ||
      decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic message_delta violated the message lifecycle"));
  }
  auto delta = required_json_object(root, "delta");
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto finish = apply_stop_reason(**delta, state, decode);
  if (!finish) {
    return finish;
  }
  return apply_anthropic_usage(root, state.response.usage);
}

[[nodiscard]] Status handle_message_stop(ProviderDecodeState& state,
                                         const AnthropicProviderDecodeState& decode,
                                         std::vector<ProviderEvent>& out) {
  if (!decode.message_started || decode.active_content_index ||
      !decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic message_stop violated the message lifecycle"));
  }
  complete_response(state, out);
  return {};
}

[[nodiscard]] Error stream_error(const JsonValue& root) {
  const auto type = error_token(root, "type").value_or("unknown_error");
  Error error = provider_error(
      error_category(type), "Anthropic stream returned an error", "anthropic:" + type);
  error.provider_request_id = request_identifier(root);
  return error;
}

[[nodiscard]] Status dispatch_event(const std::string_view type, const JsonValue& root,
                                    ProviderDecodeState& state,
                                    AnthropicProviderDecodeState& decode,
                                    std::vector<ProviderEvent>& out) {
  if (type == "message_start") {
    return handle_message_start(root, state, decode, out);
  }
  if (type == "content_block_start") {
    return handle_content_start(root, state, decode, out);
  }
  if (type == "content_block_delta") {
    return handle_content_delta(root, state, decode, out);
  }
  if (type == "content_block_stop") {
    return handle_content_stop(root, state, decode);
  }
  if (type == "message_delta") {
    return handle_message_delta(root, state, decode);
  }
  if (type == "message_stop") {
    return handle_message_stop(state, decode, out);
  }
  if (type == "error") {
    return std::unexpected(stream_error(root));
  }
  return {}; // ping
}

} // namespace

// The two adjacent string views are the shape ProviderAdapter declares.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Status AnthropicAdapter::parse_stream_event(const std::string_view event_name,
                                            const std::string_view data,
                                            ProviderDecodeState& state,
                                            std::vector<ProviderEvent>& out) const {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  auto decode = begin_event<AnthropicProviderDecodeState>(state, "Anthropic");
  if (!decode) {
    return std::unexpected(std::move(decode.error()));
  }
  // An SSE event that carries a name names the Anthropic event and its payload's
  // "type" must agree; an unnamed event arrives as "message" and is typed by that
  // payload instead. Either way an unrecognized name is ignored, not rejected.
  const auto typed_by_payload = event_name == "message";
  if (!typed_by_payload && !known_event(event_name)) {
    return {};
  }

  auto root =
      parse_json(data, ErrorCategory::protocol, "Anthropic SSE data is not valid JSON");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  auto type = required_json_string(*root, "type");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (typed_by_payload) {
    if (!known_event(*type)) {
      return {};
    }
  } else if (event_name != *type) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic SSE event name and payload type do not match"));
  }
  return dispatch_event(*type, *root, state, **decode, out);
}

} // namespace scry::detail
