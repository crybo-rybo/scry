#include "provider/openai.hpp"
#include "provider/openai_content.hpp"
#include "provider/wire_json.hpp"

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace scry::detail {
namespace {

struct MetadataFragment {
  std::string_view value;
  std::string_view field;
};

struct StreamEventView {
  std::string_view name;
  std::string_view data;
};

[[nodiscard]] Result<OpenAiProviderDecodeState*>
openai_decode_state(ProviderDecodeState& state) {
  if (std::holds_alternative<std::monostate>(state.dialect)) {
    state.dialect.emplace<OpenAiProviderDecodeState>();
  }
  auto* decode = std::get_if<OpenAiProviderDecodeState>(&state.dialect);
  if (decode == nullptr) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI stream received decode state owned by another dialect"));
  }
  return decode;
}

[[nodiscard]] Result<const WireValue*> required_object(const WireValue& owner,
                                                       const std::string_view name) {
  const auto* value = wire_field(owner, name);
  if (value == nullptr || !value->is_object()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI payload field '" + std::string{name} + "' must be an object"));
  }
  return value;
}

[[nodiscard]] Result<std::size_t> tool_index(const WireValue& value) {
  auto parsed = optional_wire_uint(value, "index");
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (!*parsed ||
      **parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI streamed tool call requires a usable index"));
  }
  return static_cast<std::size_t>(**parsed);
}

[[nodiscard]] Status assign_metadata(std::optional<std::string>& destination,
                                     const MetadataFragment fragment) {
  if (fragment.value.empty()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI streamed tool " + std::string{fragment.field} + " must not be empty"));
  }
  if (destination && *destination != fragment.value) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI streamed tool " + std::string{fragment.field} + " changed mid-stream"));
  }
  destination = fragment.value;
  return {};
}

[[nodiscard]] Status apply_optional_metadata(const WireValue& owner,
                                             const std::string_view field,
                                             std::optional<std::string>& destination) {
  auto parsed = optional_wire_string(owner, field);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  if (!*parsed) {
    return {};
  }
  return assign_metadata(destination,
                         MetadataFragment{.value = **parsed, .field = field});
}

[[nodiscard]] Status append_arguments(OpenAiToolDecodeState& tool,
                                      const std::string_view fragment,
                                      const std::size_t limit) {
  if (tool.arguments.size() > limit ||
      fragment.size() > limit - tool.arguments.size()) {
    return std::unexpected(
        make_provider_error(ErrorCategory::resource_limit,
                            "OpenAI tool arguments exceed the configured byte limit"));
  }
  tool.arguments.append(fragment);
  return {};
}

[[nodiscard]] Status apply_function_fragment(const WireValue& owner,
                                             OpenAiToolDecodeState& tool,
                                             const std::size_t limit) {
  const auto* function = wire_field(owner, "function");
  if (function == nullptr || function->is_null()) {
    return {};
  }
  if (!function->is_object()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI streamed tool function must be an object"));
  }
  auto name = apply_optional_metadata(*function, "name", tool.name);
  if (!name) {
    return name;
  }
  auto arguments = optional_wire_string(*function, "arguments");
  if (!arguments) {
    return std::unexpected(std::move(arguments.error()));
  }
  if (!*arguments) {
    return {};
  }
  return append_arguments(tool, **arguments, limit);
}

[[nodiscard]] Status apply_tool_fragment(const WireValue& value,
                                         ProviderDecodeState& state,
                                         OpenAiProviderDecodeState& decode) {
  if (!value.is_object()) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI streamed tool call fragment must be an object"));
  }
  auto index = tool_index(value);
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  auto& tool = decode.tool_calls[*index];
  auto id = apply_optional_metadata(value, "id", tool.id);
  if (!id) {
    return id;
  }
  auto type = apply_optional_metadata(value, "type", tool.type);
  if (!type) {
    return type;
  }
  if (tool.type && *tool.type != "function") {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI streamed tool call type must be function"));
  }
  auto function = apply_function_fragment(value, tool, state.max_tool_arguments_bytes);
  if (!function) {
    return function;
  }
  state.semantic_output_consumed = true;
  return {};
}

[[nodiscard]] Status apply_tool_fragments(const WireValue& delta,
                                          ProviderDecodeState& state,
                                          OpenAiProviderDecodeState& decode) {
  const auto* calls = wire_field(delta, "tool_calls");
  if (calls == nullptr || calls->is_null()) {
    return {};
  }
  if (!calls->is_array()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI streamed tool_calls must be an array"));
  }
  for (const auto& value : calls->get_array()) {
    auto applied = apply_tool_fragment(value, state, decode);
    if (!applied) {
      return applied;
    }
  }
  return {};
}

[[nodiscard]] Status finalize_tools(ProviderDecodeState& state,
                                    OpenAiProviderDecodeState& decode) {
  std::size_t expected_index = 0;
  for (auto& [index, tool] : decode.tool_calls) {
    if (index != expected_index || !tool.id || !tool.name || !tool.type ||
        *tool.type != "function") {
      return std::unexpected(make_provider_error(
          ErrorCategory::protocol,
          "OpenAI streamed tool calls are incomplete or noncontiguous"));
    }
    auto arguments = canonical_openai_arguments(tool.arguments);
    if (!arguments) {
      return std::unexpected(std::move(arguments.error()));
    }
    state.response.content.push_back(ToolCallBlock{
        .id = *tool.id,
        .name = *tool.name,
        .arguments = Json{.text = std::move(*arguments)},
    });
    ++expected_index;
  }
  decode.tools_finalized = true;
  return {};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
apply_text_delta(const WireValue& delta, ProviderDecodeState& state,
                 OpenAiProviderDecodeState& decode) {
  const auto* content = wire_field(delta, "content");
  if (content == nullptr || content->is_null()) {
    return std::vector<ProviderEvent>{};
  }
  if (!content->is_string()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI streamed content must be a string or null"));
  }
  const auto text = content->get_string();
  if (text.empty()) {
    return std::vector<ProviderEvent>{};
  }
  if (!decode.text_content_index) {
    decode.text_content_index = state.response.content.size();
    state.response.content.push_back(TextBlock{});
  }
  auto* destination =
      std::get_if<TextBlock>(&state.response.content[*decode.text_content_index]);
  if (destination == nullptr) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI streamed text targeted a non-text content block"));
  }
  destination->text.append(text);
  state.semantic_output_consumed = true;
  return std::vector<ProviderEvent>{
      ProviderTextDelta{.text = std::string{text}},
  };
}

[[nodiscard]] Status validate_delta_role(const WireValue& delta) {
  auto role = optional_wire_string(delta, "role");
  if (!role) {
    return std::unexpected(std::move(role.error()));
  }
  if (*role && **role != "assistant") {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI streamed response role must be assistant"));
  }
  const auto* legacy = wire_field(delta, "function_call");
  if (legacy != nullptr && !legacy->is_null()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI returned the unsupported deprecated function_call field"));
  }
  return {};
}

[[nodiscard]] Status validate_chunk_envelope(const WireValue& root,
                                             OpenAiProviderDecodeState& decode) {
  auto type = required_wire_string(root, "object");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type != "chat.completion.chunk") {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI stream data is not a Chat Completions chunk"));
  }
  auto id = required_wire_string(root, "id");
  if (!id) {
    return std::unexpected(std::move(id.error()));
  }
  if (id->empty()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI stream chunk ID must not be empty"));
  }
  if (decode.chunk_id && *decode.chunk_id != *id) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI stream chunk ID changed mid-stream"));
  }
  decode.chunk_id = *id;
  return {};
}

[[nodiscard]] Status validate_choice(const WireValue& choice) {
  if (!choice.is_object()) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI stream choice must be an object"));
  }
  auto index = optional_wire_uint(choice, "index");
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (!*index || **index != 0) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol, "OpenAI stream choice index must be zero"));
  }
  return {};
}

[[nodiscard]] Result<const WireValue*>
validated_choice_delta(const WireValue& choice,
                       const OpenAiProviderDecodeState& decode) {
  auto valid = validate_choice(choice);
  if (!valid) {
    return std::unexpected(std::move(valid.error()));
  }
  if (decode.finish_observed) {
    return std::unexpected(make_provider_error(
        ErrorCategory::protocol,
        "OpenAI stream emitted semantic data after its finish reason"));
  }
  auto delta = required_object(choice, "delta");
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto role = validate_delta_role(**delta);
  if (!role) {
    return std::unexpected(std::move(role.error()));
  }
  return *delta;
}

[[nodiscard]] Status apply_finish_reason(const WireValue& choice,
                                         ProviderDecodeState& state,
                                         OpenAiProviderDecodeState& decode) {
  auto reason = optional_wire_string(choice, "finish_reason");
  if (!reason) {
    return std::unexpected(std::move(reason.error()));
  }
  if (!*reason) {
    return {};
  }
  auto finish = decode_openai_finish(*reason);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  auto finalized = finalize_tools(state, decode);
  if (!finalized) {
    return std::unexpected(std::move(finalized.error()));
  }
  state.response.finish_reason = *finish;
  decode.finish_observed = true;
  return {};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
apply_choice(const WireValue& choice, ProviderDecodeState& state, const WireValue& root,
             OpenAiProviderDecodeState& decode) {
  auto delta = validated_choice_delta(choice, decode);
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto events = apply_text_delta(**delta, state, decode);
  if (!events) {
    return std::unexpected(std::move(events.error()));
  }
  auto tools = apply_tool_fragments(**delta, state, decode);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }
  auto usage = apply_openai_usage(root, state.response.usage);
  if (!usage) {
    return std::unexpected(std::move(usage.error()));
  }
  auto finish = apply_finish_reason(choice, state, decode);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  return events;
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
apply_usage_chunk(const WireValue& root, ProviderDecodeState& state,
                  const OpenAiProviderDecodeState& decode) {
  const auto* usage = wire_field(root, "usage");
  if (!decode.finish_observed || usage == nullptr || !usage->is_object()) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI empty-choice chunk must carry post-finish usage"));
  }
  auto applied = apply_openai_usage(root, state.response.usage);
  if (!applied) {
    return std::unexpected(std::move(applied.error()));
  }
  return std::vector<ProviderEvent>{};
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
apply_chunk(const WireValue& root, ProviderDecodeState& state,
            OpenAiProviderDecodeState& decode) {
  auto envelope = validate_chunk_envelope(root, decode);
  if (!envelope) {
    return std::unexpected(std::move(envelope.error()));
  }
  auto choices = required_wire_array(root, "choices");
  if (!choices) {
    return std::unexpected(std::move(choices.error()));
  }
  if ((*choices)->empty()) {
    return apply_usage_chunk(root, state, decode);
  }
  if ((*choices)->size() != 1) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI stream chunk must contain at most one choice"));
  }
  return apply_choice((*choices)->front(), state, root, decode);
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
complete_stream(ProviderDecodeState& state, const OpenAiProviderDecodeState& decode) {
  if (!decode.finish_observed || !decode.tools_finalized) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI stream ended before a complete finish reason"));
  }
  state.completed = true;
  return std::vector<ProviderEvent>{
      ProviderCompleted{.response = state.response},
  };
}

[[nodiscard]] std::optional<Error> optional_root_error(const std::string_view data,
                                                       const std::string& request_id) {
  auto root = parse_wire_json(data, ErrorCategory::protocol,
                              "OpenAI SSE data is not valid JSON");
  if (!root || !is_openai_error(*root)) {
    return std::nullopt;
  }
  return decode_openai_error(*root, "OpenAI-compatible stream returned an error",
                             request_id);
}

[[nodiscard]] Result<std::vector<ProviderEvent>>
decode_stream_event(const StreamEventView event, ProviderDecodeState& state,
                    OpenAiProviderDecodeState& decode) {
  if (event.name != "message" && event.name != "error") {
    if (auto error =
            optional_root_error(event.data, state.response.provider_request_id)) {
      return std::unexpected(std::move(*error));
    }
    if (decode.finish_observed) {
      return std::unexpected(make_provider_error(
          ErrorCategory::protocol,
          "OpenAI stream emitted an optional event after its finish reason"));
    }
    return std::vector<ProviderEvent>{
        ProviderIgnoredEvent{.name = std::string{event.name}},
    };
  }
  if (event.data == "[DONE]") {
    if (event.name != "message") {
      return std::unexpected(make_provider_error(
          ErrorCategory::protocol,
          "OpenAI named error event cannot contain the terminal marker"));
    }
    return complete_stream(state, decode);
  }
  auto root = parse_wire_json(event.data, ErrorCategory::protocol,
                              "OpenAI SSE data is not valid JSON");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  if (event.name == "error" || is_openai_error(*root)) {
    return std::unexpected(
        decode_openai_error(*root, "OpenAI-compatible stream returned an error",
                            state.response.provider_request_id));
  }
  return apply_chunk(*root, state, decode);
}

} // namespace

// The adjacent string views are fixed by the ProviderAdapter seam.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Result<std::vector<ProviderEvent>>
OpenAiAdapter::parse_stream_event(const std::string_view event_name,
                                  const std::string_view data,
                                  ProviderDecodeState& state) const {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (state.completed) {
    return std::unexpected(
        make_provider_error(ErrorCategory::protocol,
                            "OpenAI stream emitted data after its terminal event"));
  }
  auto decode = openai_decode_state(state);
  if (!decode) {
    return std::unexpected(std::move(decode.error()));
  }
  return decode_stream_event(StreamEventView{.name = event_name, .data = data}, state,
                             **decode);
}

} // namespace scry::detail
