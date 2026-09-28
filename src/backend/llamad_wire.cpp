#include "backend/llamad_wire.hpp"

#include "core/error.hpp"
#include "core/json_codec.hpp"
#include "provider/shared.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace scry::detail::llamad_wire {
namespace {

namespace wire = ::llamad::v1;

[[nodiscard]] Status invalid_request(std::string message) {
  return std::unexpected(make_error(ErrorCategory::invalid_config, std::move(message)));
}

[[nodiscard]] Status protocol_error(std::string message) {
  return std::unexpected(make_error(ErrorCategory::protocol, std::move(message)));
}

[[nodiscard]] std::string joined_text(const Message& message) {
  std::string text{};
  for (const auto& block : message.content) {
    if (const auto* part = std::get_if<TextBlock>(&block)) {
      text.append(part->text);
    }
  }
  return text;
}

[[nodiscard]] Status encode_tool_result(const ToolResultBlock& result,
                                        wire::ChatRequest& out) {
  if (result.tool_call_id.empty()) {
    return invalid_request("llamad tool results require a nonempty call ID");
  }
  if (auto status = embedded_json_value(result.result.text,
                                        "llamad tool result must be valid JSON");
      !status) {
    return status;
  }
  // is_error has no wire field: an error result's JSON already carries
  // {"error": ...}, which is what the model reads either way.
  auto* message = out.add_messages();
  message->set_role("tool");
  message->set_content(result.result.text);
  message->set_tool_call_id(result.tool_call_id);
  return {};
}

// A user message is either plain text or a run of tool results, each of which
// becomes its own "tool" message. The shape is checked before anything is
// appended.
[[nodiscard]] Status encode_user_message(const Message& message,
                                         wire::ChatRequest& out) {
  bool saw_text = false;
  bool saw_result = false;
  for (const auto& block : message.content) {
    if (std::holds_alternative<TextBlock>(block)) {
      saw_text = true;
    } else if (std::holds_alternative<ToolResultBlock>(block)) {
      saw_result = true;
    } else {
      return invalid_request("llamad user messages cannot contain tool calls");
    }
  }
  if (saw_text && saw_result) {
    return invalid_request("llamad user messages cannot mix text and tool results");
  }
  if (!saw_result) {
    auto* encoded = out.add_messages();
    encoded->set_role("user");
    encoded->set_content(joined_text(message));
    return {};
  }
  for (const auto& block : message.content) {
    if (auto status = encode_tool_result(std::get<ToolResultBlock>(block), out);
        !status) {
      return status;
    }
  }
  return {};
}

[[nodiscard]] Status encode_tool_call(const ToolCallBlock& call,
                                      wire::ChatMessage& message) {
  if (call.id.empty() || call.name.empty()) {
    return invalid_request(
        "llamad assistant tool calls require nonempty IDs and names");
  }
  if (auto status = embedded_json_object(call.arguments.text,
                                         "llamad tool arguments must be a JSON object");
      !status) {
    return status;
  }
  auto* encoded = message.add_tool_calls();
  encoded->set_id(call.id);
  encoded->set_name(call.name);
  encoded->set_arguments_json(call.arguments.text);
  return {};
}

[[nodiscard]] Status encode_assistant_message(const Message& message,
                                              wire::ChatRequest& out) {
  auto* encoded = out.add_messages();
  encoded->set_role("assistant");
  encoded->set_content(joined_text(message));
  for (const auto& block : message.content) {
    if (std::holds_alternative<TextBlock>(block)) {
      continue;
    }
    const auto* call = std::get_if<ToolCallBlock>(&block);
    if (call == nullptr) {
      return invalid_request("llamad assistant messages cannot contain tool results");
    }
    if (auto status = encode_tool_call(*call, *encoded); !status) {
      return status;
    }
  }
  return {};
}

[[nodiscard]] Status encode_tools(const ModelRequest& request, wire::ChatRequest& out) {
  if (!request.tools) {
    return {};
  }
  for (const auto& tool : *request.tools) {
    if (tool.name.empty()) {
      return invalid_request("llamad tools require a nonempty name");
    }
    if (auto status = embedded_json_object(tool.input_schema.text,
                                           "llamad tool schema must be a JSON object");
        !status) {
      return status;
    }
    auto* encoded = out.add_tools();
    encoded->set_name(tool.name);
    encoded->set_description(tool.description);
    encoded->set_parameters_json_schema(tool.input_schema.text);
  }
  return {};
}

// Validation bounded every value for llamad's float and int32 fields, so the
// narrowing casts cannot overflow.
void encode_sampling(const SamplingConfig& sampling, wire::SamplingParams& out) {
  out.set_temperature(static_cast<float>(sampling.temperature));
  if (sampling.top_p) {
    out.set_top_p(static_cast<float>(*sampling.top_p));
  }
  if (sampling.seed) {
    out.set_seed(*sampling.seed);
  }
  if (sampling.max_tokens) {
    out.set_max_tokens(static_cast<std::int32_t>(*sampling.max_tokens));
  }
}

[[nodiscard]] Usage usage_from(const wire::GenerateStats& stats) noexcept {
  const auto count = [](const std::int32_t value) {
    return static_cast<std::uint64_t>(std::max(value, std::int32_t{0}));
  };
  return Usage{
      .input_tokens = count(stats.prompt_tokens()),
      .output_tokens = count(stats.completion_tokens()),
  };
}

} // namespace

Status encode_chat_request(const ModelRequest& request, wire::ChatRequest& out) {
  if (!request.system_prompt.empty()) {
    auto* system = out.add_messages();
    system->set_role("system");
    system->set_content(request.system_prompt);
  }
  if (auto status = for_each_request_message(
          request,
          [&out](const Message& message) {
            return message.role == Role::user ? encode_user_message(message, out)
                                              : encode_assistant_message(message, out);
          });
      !status) {
    return status;
  }
  if (auto status = encode_tools(request, out); !status) {
    return status;
  }
  encode_sampling(request.sampling, *out.mutable_sampling());
  return {};
}

StreamDecoder::StreamDecoder(const ResourceLimits& limits,
                             ProviderEventSink& sink) noexcept
    : max_response_bytes_(limits.max_response_bytes),
      max_tool_arguments_bytes_(limits.max_tool_arguments_bytes), sink_(sink) {}

Status StreamDecoder::consume(const wire::GenerateChunk& chunk) {
  if (final_seen_) {
    return protocol_error("llamad stream sent a chunk after its final chunk");
  }
  if (auto status = account(chunk.ByteSizeLong()); !status) {
    return status;
  }
  if (!chunk.text().empty()) {
    if (auto status = publish_text(chunk.text()); !status) {
      return status;
    }
  }
  if (chunk.finish_reason() == wire::FINISH_REASON_UNSPECIFIED) {
    if (chunk.tool_calls_size() != 0) {
      return protocol_error("llamad stream sent tool calls before its final chunk");
    }
    return {};
  }
  final_seen_ = true;
  return consume_final(chunk);
}

Result<ModelResponse> StreamDecoder::finish() {
  if (!final_seen_) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "llamad stream ended without a final chunk"));
  }
  return std::move(response_);
}

// The serialized size of every received message counts against the response
// budget, the same bound the HTTP transport applies to body bytes.
Status StreamDecoder::account(const std::size_t bytes) {
  if (bytes > max_response_bytes_ || received_bytes_ > max_response_bytes_ - bytes) {
    return std::unexpected(
        make_error(ErrorCategory::resource_limit, "response exceeds configured limit"));
  }
  received_bytes_ += bytes;
  return {};
}

Status StreamDecoder::report_semantic_output() {
  if (semantic_output_reported_) {
    return {};
  }
  semantic_output_reported_ = true;
  return sink_(ProviderSemanticOutput{});
}

Status StreamDecoder::publish_text(const std::string& text) {
  if (auto status = report_semantic_output(); !status) {
    return status;
  }
  text_.append(text);
  return sink_(ProviderTextDelta{.text = text});
}

Status StreamDecoder::consume_final(const wire::GenerateChunk& chunk) {
  response_.usage = usage_from(chunk.stats());
  switch (chunk.finish_reason()) {
  case wire::FINISH_REASON_EOG:
  case wire::FINISH_REASON_STOP:
    return finish_without_tools(chunk, FinishReason::completed);
  case wire::FINISH_REASON_LENGTH:
    return finish_without_tools(chunk, FinishReason::length);
  case wire::FINISH_REASON_TOOL_CALLS:
    return finish_with_tools(chunk);
  case wire::FINISH_REASON_CANCELLED:
    return std::unexpected(
        make_error(ErrorCategory::cancelled, "llamad cancelled the generation"));
  default:
    return protocol_error("llamad stream ended with an unknown finish reason");
  }
}

Status StreamDecoder::finish_without_tools(const wire::GenerateChunk& chunk,
                                           const FinishReason reason) {
  if (chunk.tool_calls_size() != 0) {
    return protocol_error(
        "llamad stream sent tool calls without the tool-calls finish reason");
  }
  if (!text_.empty()) {
    response_.content.emplace_back(TextBlock{.text = std::move(text_)});
  }
  response_.finish_reason = reason;
  return {};
}

// Tool calls arrive whole on the final chunk, never as fragments, so each is
// checked once here. The arguments are forwarded as received; the turn machine
// owns the single canonicalization pass.
Status StreamDecoder::finish_with_tools(const wire::GenerateChunk& chunk) {
  if (chunk.tool_calls_size() == 0) {
    return protocol_error("llamad stream finished for tool calls without any");
  }
  if (auto status = report_semantic_output(); !status) {
    return status;
  }
  if (!text_.empty()) {
    response_.content.emplace_back(TextBlock{.text = std::move(text_)});
  }
  for (const auto& call : chunk.tool_calls()) {
    if (call.id().empty() || call.name().empty()) {
      return protocol_error("llamad tool call is missing its ID or name");
    }
    if (call.arguments_json().size() > max_tool_arguments_bytes_) {
      return std::unexpected(
          make_error(ErrorCategory::resource_limit,
                     "llamad tool arguments exceed the configured byte limit"));
    }
    if (auto status =
            validate_json_object(call.arguments_json(), ErrorCategory::protocol,
                                 "llamad tool arguments must be a JSON object");
        !status) {
      return status;
    }
    response_.content.emplace_back(ToolCallBlock{
        .id = call.id(),
        .name = call.name(),
        .arguments = Json{.text = call.arguments_json()},
    });
  }
  response_.finish_reason = FinishReason::tool_use;
  return {};
}

} // namespace scry::detail::llamad_wire
