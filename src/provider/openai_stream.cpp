#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/openai.hpp"
#include "provider/shared.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace scry::detail {
namespace {

// Deliberately divergent from the Anthropic adapter: OpenAI usage objects
// replace the running totals, so a missing field zeroes the count instead of
// preserving the previous value.
[[nodiscard]] Status assign_usage_count(const JsonValue& usage,
                                        const std::string_view field,
                                        std::uint64_t& destination) {
  auto count = optional_json_uint(usage, field);
  if (!count) {
    return std::unexpected(std::move(count.error()));
  }
  destination = count->value_or(0);
  return {};
}

[[nodiscard]] Status apply_usage(const JsonValue& owner, Usage& usage) {
  const auto* value = json_field(owner, "usage");
  if (value == nullptr || value->is_null()) {
    return {};
  }
  if (!value->is_object()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "OpenAI usage must be an object or null"));
  }
  auto input = assign_usage_count(*value, "prompt_tokens", usage.input_tokens);
  if (!input) {
    return input;
  }
  return assign_usage_count(*value, "completion_tokens", usage.output_tokens);
}

[[nodiscard]] Result<FinishReason> decode_finish(const std::string_view reason) {
  if (reason == "stop") {
    return FinishReason::completed;
  }
  if (reason == "length") {
    return FinishReason::length;
  }
  if (reason == "tool_calls") {
    return FinishReason::tool_use;
  }
  if (reason == "function_call") {
    return std::unexpected(make_error(
        ErrorCategory::protocol,
        "OpenAI returned the unsupported deprecated function_call finish reason"));
  }
  return FinishReason::unknown;
}

// OpenAI's own aliases, then the error types both dialects share.
[[nodiscard]] ErrorCategory
openai_error_category(const std::string_view token) noexcept {
  if (token == "invalid_api_key") {
    return ErrorCategory::authentication;
  }
  if (token == "rate_limit_exceeded" || token == "insufficient_quota") {
    return ErrorCategory::rate_limit;
  }
  if (token == "server_error") {
    return ErrorCategory::network;
  }
  return error_category(token);
}

[[nodiscard]] bool is_error_root(const JsonValue& root) noexcept {
  return json_field(root, "error") != nullptr;
}

// Picks the token that names the error. A "type" or "code" string that maps to a
// specific category wins, type first; otherwise the first of the two that is
// present labels a protocol error, and a numeric "code" is the last resort.
[[nodiscard]] Error stream_error(const JsonValue& root) {
  constexpr auto message =
      std::string_view{"OpenAI-compatible stream returned an error"};
  const auto type = error_token(root, "type");
  const auto code = error_token(root, "code");
  for (const auto* token : {&type, &code}) {
    if (*token) {
      const auto category = openai_error_category(**token);
      if (category != ErrorCategory::protocol) {
        return provider_error(category, message, "openai:" + **token);
      }
    }
  }
  std::string token = type ? *type : code.value_or("unknown_error");
  const auto* error = json_field(root, "error");
  const auto* numeric = error == nullptr ? nullptr : json_field(*error, "code");
  if (!type && !code && numeric != nullptr && numeric->is_uint64()) {
    token = std::to_string(numeric->get<std::uint64_t>());
  }
  return provider_error(ErrorCategory::protocol, message, "openai:" + token);
}

// Records a streamed tool id or name. Either may repeat across fragments, but
// never empty and never changed.
[[nodiscard]] Status apply_tool_string(const JsonValue& owner,
                                       const std::string_view field,
                                       std::string& destination) {
  auto parsed = optional_json_string(owner, field);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  const auto value = *parsed;
  if (!value) {
    return {};
  }
  if (value->empty()) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI streamed tool " + std::string{field} +
                                          " must not be empty"));
  }
  if (!destination.empty() && destination != *value) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI streamed tool " + std::string{field} +
                                          " changed mid-stream"));
  }
  destination = *value;
  return {};
}

[[nodiscard]] Status apply_tool_type(const JsonValue& owner,
                                     OpenAiToolDecodeState& tool) {
  auto parsed = optional_json_string(owner, "type");
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }
  const auto parsed_type = *parsed;
  if (!parsed_type) {
    return {};
  }
  const auto type = *parsed_type;
  if (type.empty()) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI streamed tool type must not be empty"));
  }
  if (type != "function") {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI streamed tool call type must be function"));
  }
  tool.typed = true;
  return {};
}

[[nodiscard]] Status apply_function_fragment(const JsonValue& owner,
                                             OpenAiToolDecodeState& tool,
                                             const std::size_t limit) {
  const auto* function = json_field(owner, "function");
  if (function == nullptr || function->is_null()) {
    return {};
  }
  if (!function->is_object()) {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI streamed tool function must be an object"));
  }
  auto name = apply_tool_string(*function, "name", tool.name);
  if (!name) {
    return name;
  }
  auto arguments = optional_json_string(*function, "arguments");
  if (!arguments) {
    return std::unexpected(std::move(arguments.error()));
  }
  const auto fragment = *arguments;
  if (!fragment) {
    return {};
  }
  return append_tool_arguments(
      tool.arguments, *fragment, limit,
      "OpenAI tool arguments exceed the configured byte limit");
}

[[nodiscard]] OpenAiToolDecodeState& indexed_tool(OpenAiProviderDecodeState& decode,
                                                  const std::size_t index) {
  auto position = std::lower_bound(
      decode.tool_calls.begin(), decode.tool_calls.end(), index,
      [](const OpenAiToolDecodeState& tool, const std::size_t candidate) {
        return tool.index < candidate;
      });
  if (position == decode.tool_calls.end() || position->index != index) {
    position =
        decode.tool_calls.insert(position, OpenAiToolDecodeState{.index = index});
  }
  return *position;
}

[[nodiscard]] Status apply_tool_fragment(const JsonValue& value,
                                         ProviderDecodeState& state,
                                         OpenAiProviderDecodeState& decode) {
  if (!value.is_object()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI streamed tool call fragment must be an object"));
  }
  auto index =
      required_index(value, "OpenAI streamed tool call requires a usable index");
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  auto& tool = indexed_tool(decode, *index);
  auto id = apply_tool_string(value, "id", tool.id);
  if (!id) {
    return id;
  }
  auto type = apply_tool_type(value, tool);
  if (!type) {
    return type;
  }
  auto function = apply_function_fragment(value, tool, state.max_tool_arguments_bytes);
  if (!function) {
    return function;
  }
  state.semantic_output_consumed = true;
  return {};
}

[[nodiscard]] Status apply_tool_fragments(const JsonValue& delta,
                                          ProviderDecodeState& state,
                                          OpenAiProviderDecodeState& decode) {
  const auto* calls = json_field(delta, "tool_calls");
  if (calls == nullptr || calls->is_null()) {
    return {};
  }
  if (!calls->is_array()) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI streamed tool_calls must be an array"));
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
  for (auto& tool : decode.tool_calls) {
    if (tool.index != expected_index || tool.id.empty() || tool.name.empty() ||
        !tool.typed) {
      return std::unexpected(
          make_error(ErrorCategory::protocol,
                     "OpenAI streamed tool calls are incomplete or noncontiguous"));
    }
    if (tool.arguments.empty()) {
      tool.arguments = "{}";
    }
    // The stream layer only proves the arguments are a JSON object and
    // forwards the bytes as received; TurnMachine owns the single
    // canonicalization pass.
    if (auto status = validate_json_object(
            tool.arguments, ErrorCategory::protocol,
            "OpenAI streamed tool arguments must be a JSON object");
        !status) {
      return status;
    }
    state.response.content.push_back(ToolCallBlock{
        .id = std::move(tool.id),
        .name = std::move(tool.name),
        .arguments = Json{.text = std::move(tool.arguments)},
    });
    ++expected_index;
  }
  return {};
}

[[nodiscard]] Status apply_text_delta(const JsonValue& delta,
                                      ProviderDecodeState& state,
                                      std::vector<ProviderEvent>& out) {
  const auto* content = json_field(delta, "content");
  if (content == nullptr || content->is_null()) {
    return {};
  }
  if (!content->is_string()) {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI streamed content must be a string or null"));
  }
  const auto text = content->get_string();
  if (text.empty()) {
    return {};
  }
  // Tool calls join the content only at the finish reason, after which every
  // delta is refused, so until then the content is empty or one text block.
  if (state.response.content.empty()) {
    state.response.content.push_back(TextBlock{});
  }
  auto* destination = std::get_if<TextBlock>(&state.response.content.front());
  if (destination == nullptr) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI streamed text targeted a non-text content block"));
  }
  destination->text.append(text);
  state.semantic_output_consumed = true;
  out.push_back(ProviderTextDelta{.text = std::string{text}});
  return {};
}

[[nodiscard]] Status validate_delta_role(const JsonValue& delta) {
  auto role = optional_json_string(delta, "role");
  if (!role) {
    return std::unexpected(std::move(role.error()));
  }
  const auto name = *role;
  if (name && *name != "assistant") {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI streamed response role must be assistant"));
  }
  const auto* legacy = json_field(delta, "function_call");
  if (legacy != nullptr && !legacy->is_null()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI returned the unsupported deprecated function_call field"));
  }
  return {};
}

[[nodiscard]] Status validate_chunk_envelope(const JsonValue& root,
                                             OpenAiProviderDecodeState& decode) {
  auto type = required_json_string(root, "object");
  if (!type) {
    return std::unexpected(std::move(type.error()));
  }
  if (*type != "chat.completion.chunk") {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI stream data is not a Chat Completions chunk"));
  }
  auto id = required_json_string(root, "id");
  if (!id) {
    return std::unexpected(std::move(id.error()));
  }
  if (id->empty()) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI stream chunk ID must not be empty"));
  }
  if (decode.chunk_id.empty()) {
    decode.chunk_id = *id;
  } else if (decode.chunk_id != *id) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI stream chunk ID changed mid-stream"));
  }
  return {};
}

[[nodiscard]] Result<const JsonValue*>
validated_choice_delta(const JsonValue& choice,
                       const OpenAiProviderDecodeState& decode) {
  if (!choice.is_object()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "OpenAI stream choice must be an object"));
  }
  auto index = optional_json_uint(choice, "index");
  if (!index) {
    return std::unexpected(std::move(index.error()));
  }
  if (index->value_or(1) != 0) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "OpenAI stream choice index must be zero"));
  }
  if (decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI stream emitted semantic data after its finish reason"));
  }
  auto delta = required_json_object(choice, "delta");
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto role = validate_delta_role(**delta);
  if (!role) {
    return std::unexpected(std::move(role.error()));
  }
  return *delta;
}

[[nodiscard]] Status apply_finish_reason(const JsonValue& choice,
                                         ProviderDecodeState& state,
                                         OpenAiProviderDecodeState& decode) {
  auto reason = optional_json_string(choice, "finish_reason");
  if (!reason) {
    return std::unexpected(std::move(reason.error()));
  }
  const auto& finish_reason = *reason;
  if (!finish_reason.has_value()) {
    return {};
  }
  auto finish = decode_finish(*finish_reason);
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

[[nodiscard]] Status apply_choice(const JsonValue& choice, ProviderDecodeState& state,
                                  const JsonValue& root,
                                  OpenAiProviderDecodeState& decode,
                                  std::vector<ProviderEvent>& out) {
  auto delta = validated_choice_delta(choice, decode);
  if (!delta) {
    return std::unexpected(std::move(delta.error()));
  }
  auto events = apply_text_delta(**delta, state, out);
  if (!events) {
    return std::unexpected(std::move(events.error()));
  }
  auto tools = apply_tool_fragments(**delta, state, decode);
  if (!tools) {
    return std::unexpected(std::move(tools.error()));
  }
  auto usage = apply_usage(root, state.response.usage);
  if (!usage) {
    return std::unexpected(std::move(usage.error()));
  }
  auto finish = apply_finish_reason(choice, state, decode);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  return {};
}

[[nodiscard]] Status apply_usage_chunk(const JsonValue& root,
                                       ProviderDecodeState& state,
                                       const OpenAiProviderDecodeState& decode) {
  const auto* usage = json_field(root, "usage");
  if (!decode.finish_observed || usage == nullptr || !usage->is_object()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI empty-choice chunk must carry post-finish usage"));
  }
  return apply_usage(root, state.response.usage);
}

[[nodiscard]] Status apply_chunk(const JsonValue& root, ProviderDecodeState& state,
                                 OpenAiProviderDecodeState& decode,
                                 std::vector<ProviderEvent>& out) {
  auto envelope = validate_chunk_envelope(root, decode);
  if (!envelope) {
    return std::unexpected(std::move(envelope.error()));
  }
  auto choices = required_json_array(root, "choices");
  if (!choices) {
    return std::unexpected(std::move(choices.error()));
  }
  if ((*choices)->empty()) {
    return apply_usage_chunk(root, state, decode);
  }
  if ((*choices)->size() != 1) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI stream chunk must contain at most one choice"));
  }
  return apply_choice((*choices)->front(), state, root, decode, out);
}

[[nodiscard]] Status complete_stream(ProviderDecodeState& state,
                                     const OpenAiProviderDecodeState& decode,
                                     std::vector<ProviderEvent>& out) {
  if (!decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI stream ended before a complete finish reason"));
  }
  complete_response(state, out);
  return {};
}

// A named event other than `message` or `error` is optional and ignored, unless
// its payload is an error root that a server mislabeled.
[[nodiscard]] Status handle_optional_event(const std::string_view data,
                                           const OpenAiProviderDecodeState& decode) {
  auto root =
      parse_json(data, ErrorCategory::protocol, "OpenAI SSE data is not valid JSON");
  if (root && is_error_root(*root)) {
    return std::unexpected(stream_error(*root));
  }
  if (decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI stream emitted an optional event after its finish reason"));
  }
  return {};
}

} // namespace

// The two adjacent string views are the shape ProviderAdapter declares.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Status OpenAiAdapter::parse_stream_event(const std::string_view event_name,
                                         const std::string_view data,
                                         ProviderDecodeState& state,
                                         std::vector<ProviderEvent>& out) const {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  auto decode = begin_event<OpenAiProviderDecodeState>(state, "OpenAI");
  if (!decode) {
    return std::unexpected(std::move(decode.error()));
  }
  if (event_name != "message" && event_name != "error") {
    return handle_optional_event(data, **decode);
  }
  if (data == "[DONE]") {
    if (event_name != "message") {
      return std::unexpected(
          make_error(ErrorCategory::protocol,
                     "OpenAI named error event cannot contain the terminal marker"));
    }
    return complete_stream(state, **decode, out);
  }
  auto root =
      parse_json(data, ErrorCategory::protocol, "OpenAI SSE data is not valid JSON");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  if (event_name == "error" || is_error_root(*root)) {
    return std::unexpected(stream_error(*root));
  }
  return apply_chunk(*root, state, **decode, out);
}

} // namespace scry::detail
