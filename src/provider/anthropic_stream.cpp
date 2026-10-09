#include "kernel/error.hpp"
#include "kernel/json/codec.hpp"
#include "provider/anthropic.hpp"
#include "provider/anthropic_content.hpp"
#include "provider/shared.hpp"

#include <cstddef>
#include <initializer_list>
#include <meta>
#include <optional>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

// Carries only the discriminator, so an event can be matched against its SSE
// name, or ignored as unknown, before its payload is decoded.
struct[[= reflection::ignore_unknown]] AnthropicEnvelope {
  std::string type;
};

// Whether `tag` is the tag of one of the variant's alternatives. It is a template
// because GCC 16 reports each expansion of a non-dependent `template for` as
// shadowing the previous one.
template <typename Variant>
[[nodiscard]] bool is_alternative_tag(const std::string_view tag) noexcept {
  bool found = false;
  template for (constexpr std::meta::info alternative :
                reflection::detail::alternatives_v<Variant>) {
    found = found || tag == *reflection::detail::tag_of(alternative);
  }
  return found;
}

// The Anthropic events Scry knows are exactly the alternatives of AnthropicEvent.
[[nodiscard]] bool known_event(const std::string_view name) noexcept {
  return is_alternative_tag<AnthropicEvent>(name);
}

// Everything one event's handler reads or changes.
struct EventContext {
  const JsonView& root;
  ProviderDecodeState& state;
  AnthropicProviderDecodeState& decode;
  std::vector<ProviderEvent>& out;
};

// A best-effort read: an identifier that is absent or not a string is empty.
[[nodiscard]] std::string
request_identifier(const JsonView& root,
                   const std::initializer_list<std::string_view> path) {
  return std::string{string_at(root, path).value_or("")};
}

// Appends a decoded block to the response, publishing any text it starts with.
// A skipped thinking block only counts toward the next block's index.
[[nodiscard]] Status push_block(AnthropicContent content, const bool streaming_start,
                                EventContext& context) {
  auto block = anthropic_content_block(std::move(content), streaming_start);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }
  if (!*block) {
    ++context.decode.skipped_blocks;
    return {};
  }
  if (const auto* text = std::get_if<TextBlock>(&**block);
      text != nullptr && !text->text.empty()) {
    context.out.push_back(ProviderTextDelta{.text = text->text});
  }
  context.state.response.content.push_back(std::move(**block));
  context.state.semantic_output_consumed = true;
  return {};
}

// Applies `stop_reason`, present on both message_start's message and
// message_delta's delta; a non-null reason marks the message finished.
void apply_stop_reason(const std::optional<std::string>& reason,
                       EventContext& context) {
  context.state.response.finish_reason = decode_anthropic_finish(reason);
  context.decode.finish_observed = reason.has_value();
}

[[nodiscard]] Status handle(AnthropicMessageStart& event, EventContext& context) {
  if (context.decode.message_started) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic stream emitted more than one message_start"));
  }
  auto& message = event.message;
  if (message.type != "message") {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "Anthropic message_start is not a message"));
  }
  if (context.state.response.provider_request_id.empty()) {
    context.state.response.provider_request_id =
        request_identifier(context.root, {"message", "request_id"});
  }
  apply_anthropic_usage(message.usage, context.state.response.usage);
  apply_stop_reason(message.stop_reason, context);
  context.decode.message_started = true;
  for (auto& content : message.content) {
    if (auto status = push_block(std::move(content), false, context); !status) {
      return status;
    }
  }
  return {};
}

[[nodiscard]] Status handle(AnthropicContentBlockStart& event, EventContext& context) {
  if (!context.decode.message_started || context.decode.active_content_index ||
      context.decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content block began outside the message lifecycle"));
  }
  if (event.index !=
      context.state.response.content.size() + context.decode.skipped_blocks) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content blocks did not begin in contiguous order"));
  }
  const auto skipped = anthropic_skipped_block(event.content_block);
  if (auto status = push_block(std::move(event.content_block), true, context);
      !status) {
    return status;
  }
  context.decode.active_content_index = event.index;
  context.decode.active_skipped = skipped;
  return {};
}

// The active block, or null when it is a skipped thinking block. The active
// block is always the last to begin, so a response block is the last one.
[[nodiscard]] Result<ContentBlock*> indexed_block(const std::size_t index,
                                                  EventContext& context) {
  if (index >= context.state.response.content.size() + context.decode.skipped_blocks) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content event referenced an unknown block"));
  }
  if (context.decode.active_content_index != index) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic content event targeted a block that is not active"));
  }
  if (context.decode.active_skipped != AnthropicSkippedBlock::none) {
    return nullptr;
  }
  return &context.state.response.content.back();
}

[[nodiscard]] Status apply_delta(AnthropicTextDelta& delta, ContentBlock* block,
                                 EventContext& context) {
  auto* destination = std::get_if<TextBlock>(block);
  if (destination == nullptr) {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "Anthropic text delta targeted a non-text block"));
  }
  destination->text.append(delta.text);
  context.state.semantic_output_consumed = true;
  context.out.push_back(ProviderTextDelta{.text = std::move(delta.text)});
  return {};
}

[[nodiscard]] Status apply_delta(const AnthropicInputJsonDelta& delta,
                                 ContentBlock* block, EventContext& context) {
  auto* destination = std::get_if<ToolCallBlock>(block);
  if (destination == nullptr) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic input JSON delta targeted a non-tool block"));
  }
  if (auto status = append_tool_arguments(
          destination->arguments.text, delta.partial_json,
          context.state.max_tool_arguments_bytes,
          "Anthropic tool arguments exceed the configured byte limit");
      !status) {
    return status;
  }
  context.state.semantic_output_consumed = true;
  return {};
}

// Scry drops thinking text and signatures, so it keeps no thinking bytes and
// consumes no semantic output.
[[nodiscard]] Status skip_thinking_delta(const EventContext& context) {
  if (context.decode.active_skipped != AnthropicSkippedBlock::thinking) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic thinking delta targeted a non-thinking block"));
  }
  return {};
}

[[nodiscard]] Status apply_delta(const AnthropicThinkingDelta&, ContentBlock*,
                                 EventContext& context) {
  return skip_thinking_delta(context);
}

[[nodiscard]] Status apply_delta(const AnthropicSignatureDelta&, ContentBlock*,
                                 EventContext& context) {
  return skip_thinking_delta(context);
}

[[nodiscard]] Status handle(AnthropicContentBlockDelta& event, EventContext& context) {
  auto block = indexed_block(event.index, context);
  if (!block) {
    return std::unexpected(std::move(block.error()));
  }
  return std::visit(
      [&block, &context](auto& delta) { return apply_delta(delta, *block, context); },
      event.delta);
}

[[nodiscard]] Status handle(const AnthropicContentBlockStop& event,
                            EventContext& context) {
  auto block = indexed_block(event.index, context);
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
  context.decode.active_content_index.reset();
  return {};
}

[[nodiscard]] Status handle(const AnthropicMessageDelta& event, EventContext& context) {
  if (!context.decode.message_started || context.decode.active_content_index ||
      context.decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic message_delta violated the message lifecycle"));
  }
  apply_stop_reason(event.delta.stop_reason, context);
  apply_anthropic_usage(event.usage, context.state.response.usage);
  return {};
}

[[nodiscard]] Status handle(const AnthropicMessageStop&, EventContext& context) {
  if (!context.decode.message_started || context.decode.active_content_index ||
      !context.decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic message_stop violated the message lifecycle"));
  }
  complete_response(context.state, context.out);
  return {};
}

// The provider's message and the raw body are never surfaced: only the
// sanitized `error.type` token and the request identifier are read.
[[nodiscard]] Status handle(const AnthropicErrorEvent&, EventContext& context) {
  const auto type = error_token(context.root, "type").value_or("unknown_error");
  Error error = provider_error(
      error_category(type), "Anthropic stream returned an error", "anthropic:" + type);
  error.provider_request_id = request_identifier(context.root, {"request_id"});
  return std::unexpected(std::move(error));
}

[[nodiscard]] Status handle(const AnthropicPing&, EventContext&) { return {}; }

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

  auto root = parse_payload(data, "Anthropic");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  auto envelope = decode_payload<AnthropicEnvelope>(*root, "Anthropic");
  if (!envelope) {
    return std::unexpected(std::move(envelope.error()));
  }
  if (typed_by_payload) {
    if (!known_event(envelope->type)) {
      return {};
    }
  } else if (event_name != envelope->type) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "Anthropic SSE event name and payload type do not match"));
  }

  auto event = decode_payload<AnthropicEvent>(*root, "Anthropic");
  if (!event) {
    return std::unexpected(std::move(event.error()));
  }
  EventContext context{.root = *root, .state = state, .decode = **decode, .out = out};
  return std::visit([&context](auto& payload) { return handle(payload, context); },
                    *event);
}

} // namespace scry::detail
