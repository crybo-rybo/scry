#include "kernel/error.hpp"
#include "kernel/json/codec.hpp"
#include "provider/openai.hpp"
#include "provider/shared.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <scry/annotations.hpp>
#include <scry/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace scry::detail {
namespace {

// Chat Completions stream chunks as reflected aggregates. Every class ignores
// members it does not declare, because servers add fields, and reads an absent or
// null optional member as disengaged. What the codec cannot express - chunk
// identity, fragment accumulation, the finish lifecycle - stays below.

struct[
    [ = reflection::ignore_unknown, = reflection::skip_null ]] OpenAiFunctionFragment {
  std::optional<std::string> name;
  std::optional<std::string> arguments;
};

struct[[ = reflection::ignore_unknown, = reflection::skip_null ]] OpenAiToolFragment {
  std::size_t index;
  std::optional<std::string> id;
  std::optional<std::string> type;
  std::optional<OpenAiFunctionFragment> function;
};

// `function_call` is the deprecated legacy field: any value but null is refused.
struct[[ = reflection::ignore_unknown, = reflection::skip_null ]] OpenAiDelta {
  std::optional<std::string> role;
  std::optional<std::string> content;
  std::optional<std::vector<OpenAiToolFragment>> tool_calls;
  std::optional<Json> function_call;
};

struct[[ = reflection::ignore_unknown, = reflection::skip_null ]] OpenAiChoice {
  std::uint64_t index;
  OpenAiDelta delta;
  std::optional<std::string> finish_reason;
};

// Only non-negative integers that fit 64 bits are counts; null reads as absent.
struct[[ = reflection::ignore_unknown, = reflection::skip_null ]] OpenAiUsage {
  std::optional<std::uint64_t> prompt_tokens;
  std::optional<std::uint64_t> completion_tokens;
};

struct[[ = reflection::ignore_unknown, = reflection::skip_null ]] OpenAiChunk {
  std::string id;
  std::string object;
  std::vector<OpenAiChoice> choices;
  std::optional<OpenAiUsage> usage;
};

// Deliberately divergent from the Anthropic adapter: OpenAI usage objects
// replace the running totals, so a missing field zeroes the count instead of
// preserving the previous value.
void apply_usage(const std::optional<OpenAiUsage>& reported, Usage& usage) {
  if (reported) {
    usage.input_tokens = reported->prompt_tokens.value_or(0);
    usage.output_tokens = reported->completion_tokens.value_or(0);
  }
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

// A payload carrying an `error` member, of any value, is an error however the
// event was named.
[[nodiscard]] bool is_error_root(const JsonView& root) {
  return root.find("error").has_value();
}

// Picks the token that names the error. A "type" or "code" string that maps to a
// specific category wins, type first; otherwise the first of the two that is
// present labels a protocol error, and a numeric "code" is the last resort. The
// provider's message and the raw body are never surfaced.
[[nodiscard]] Error stream_error(const JsonView& root) {
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
  const auto numeric =
      member_at(root, {"error", "code"}).and_then([](const JsonView& value) {
        return value.unsigned_integer();
      });
  if (!type && !code && numeric) {
    token = std::to_string(*numeric);
  }
  return provider_error(ErrorCategory::protocol, message, "openai:" + token);
}

// Records a streamed tool id or name. Either may repeat across fragments, but
// never empty and never changed.
[[nodiscard]] Status apply_tool_string(const std::optional<std::string>& value,
                                       const std::string_view field,
                                       std::string& destination) {
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

[[nodiscard]] Status apply_tool_type(const std::optional<std::string>& type,
                                     OpenAiToolDecodeState& tool) {
  if (!type) {
    return {};
  }
  if (type->empty()) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI streamed tool type must not be empty"));
  }
  if (*type != "function") {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI streamed tool call type must be function"));
  }
  tool.typed = true;
  return {};
}

[[nodiscard]] Status
apply_function_fragment(const std::optional<OpenAiFunctionFragment>& function,
                        OpenAiToolDecodeState& tool, const std::size_t limit) {
  if (!function) {
    return {};
  }
  if (auto name = apply_tool_string(function->name, "name", tool.name); !name) {
    return name;
  }
  if (!function->arguments) {
    return {};
  }
  return append_tool_arguments(
      tool.arguments, *function->arguments, limit,
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

[[nodiscard]] Status apply_tool_fragment(const OpenAiToolFragment& fragment,
                                         ProviderDecodeState& state,
                                         OpenAiProviderDecodeState& decode) {
  auto& tool = indexed_tool(decode, fragment.index);
  if (auto id = apply_tool_string(fragment.id, "id", tool.id); !id) {
    return id;
  }
  if (auto type = apply_tool_type(fragment.type, tool); !type) {
    return type;
  }
  if (auto function = apply_function_fragment(fragment.function, tool,
                                              state.max_tool_arguments_bytes);
      !function) {
    return function;
  }
  state.semantic_output_consumed = true;
  return {};
}

[[nodiscard]] Status
apply_tool_fragments(const std::optional<std::vector<OpenAiToolFragment>>& fragments,
                     ProviderDecodeState& state, OpenAiProviderDecodeState& decode) {
  if (!fragments) {
    return {};
  }
  for (const auto& fragment : *fragments) {
    if (auto applied = apply_tool_fragment(fragment, state, decode); !applied) {
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

[[nodiscard]] Status apply_text_delta(std::optional<std::string>& content,
                                      ProviderDecodeState& state,
                                      std::vector<ProviderEvent>& out) {
  if (!content || content->empty()) {
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
  destination->text.append(*content);
  state.semantic_output_consumed = true;
  out.push_back(ProviderTextDelta{.text = std::move(*content)});
  return {};
}

[[nodiscard]] Status validate_delta_role(const OpenAiDelta& delta) {
  if (delta.role && *delta.role != "assistant") {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI streamed response role must be assistant"));
  }
  if (delta.function_call) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI returned the unsupported deprecated function_call field"));
  }
  return {};
}

[[nodiscard]] Status validate_chunk_envelope(const OpenAiChunk& chunk,
                                             OpenAiProviderDecodeState& decode) {
  if (chunk.object != "chat.completion.chunk") {
    return std::unexpected(make_error(
        ErrorCategory::protocol, "OpenAI stream data is not a Chat Completions chunk"));
  }
  if (chunk.id.empty()) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI stream chunk ID must not be empty"));
  }
  if (decode.chunk_id.empty()) {
    decode.chunk_id = chunk.id;
  } else if (decode.chunk_id != chunk.id) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "OpenAI stream chunk ID changed mid-stream"));
  }
  return {};
}

[[nodiscard]] Status validate_choice(const OpenAiChoice& choice,
                                     const OpenAiProviderDecodeState& decode) {
  if (choice.index != 0) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "OpenAI stream choice index must be zero"));
  }
  if (decode.finish_observed) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI stream emitted semantic data after its finish reason"));
  }
  return validate_delta_role(choice.delta);
}

[[nodiscard]] Status apply_finish_reason(const std::optional<std::string>& reason,
                                         ProviderDecodeState& state,
                                         OpenAiProviderDecodeState& decode) {
  if (!reason) {
    return {};
  }
  auto finish = decode_finish(*reason);
  if (!finish) {
    return std::unexpected(std::move(finish.error()));
  }
  if (auto finalized = finalize_tools(state, decode); !finalized) {
    return finalized;
  }
  state.response.finish_reason = *finish;
  decode.finish_observed = true;
  return {};
}

[[nodiscard]] Status apply_choice(OpenAiChunk& chunk, ProviderDecodeState& state,
                                  OpenAiProviderDecodeState& decode,
                                  std::vector<ProviderEvent>& out) {
  auto& choice = chunk.choices.front();
  if (auto valid = validate_choice(choice, decode); !valid) {
    return valid;
  }
  if (auto text = apply_text_delta(choice.delta.content, state, out); !text) {
    return text;
  }
  if (auto tools = apply_tool_fragments(choice.delta.tool_calls, state, decode);
      !tools) {
    return tools;
  }
  apply_usage(chunk.usage, state.response.usage);
  return apply_finish_reason(choice.finish_reason, state, decode);
}

[[nodiscard]] Status apply_chunk(OpenAiChunk& chunk, ProviderDecodeState& state,
                                 OpenAiProviderDecodeState& decode,
                                 std::vector<ProviderEvent>& out) {
  if (auto envelope = validate_chunk_envelope(chunk, decode); !envelope) {
    return envelope;
  }
  if (chunk.choices.empty()) {
    // An empty-choice chunk is the trailing usage report, allowed only after the
    // finish reason.
    if (!decode.finish_observed || !chunk.usage) {
      return std::unexpected(
          make_error(ErrorCategory::protocol,
                     "OpenAI empty-choice chunk must carry post-finish usage"));
    }
    apply_usage(chunk.usage, state.response.usage);
    return {};
  }
  if (chunk.choices.size() != 1) {
    return std::unexpected(
        make_error(ErrorCategory::protocol,
                   "OpenAI stream chunk must contain at most one choice"));
  }
  return apply_choice(chunk, state, decode, out);
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

// A named event other than `message` or `error` is optional: ignored before the
// finish reason and refused after it. A payload that is an error root is an error
// a server mislabeled and surfaces as the stream error either way.
[[nodiscard]] Status handle_optional_event(const std::string_view data,
                                           const OpenAiProviderDecodeState& decode) {
  auto root = parse_payload(data, "OpenAI");
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
  auto root = parse_payload(data, "OpenAI");
  if (!root) {
    return std::unexpected(std::move(root.error()));
  }
  if (event_name == "error" || is_error_root(*root)) {
    return std::unexpected(stream_error(*root));
  }
  auto chunk = decode_payload<OpenAiChunk>(*root, "OpenAI");
  if (!chunk) {
    return std::unexpected(std::move(chunk.error()));
  }
  return apply_chunk(*chunk, state, **decode, out);
}

} // namespace scry::detail
