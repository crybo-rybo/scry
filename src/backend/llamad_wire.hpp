#pragma once

// The mapping between Scry's neutral model and llamad's wire contract
// (proto/llamad/v1/llamad.proto): the ChatRequest encoder and the decoder for
// the GenerateChunk stream. It knows protobuf messages but no gRPC, so it is
// testable without a channel. Compiled only with SCRY_WITH_LLAMAD.

#include "core/backend.hpp"
#include "core/model.hpp"

#include <cstddef>
#include <llamad/v1/llamad.pb.h>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <string>
#include <string_view>

namespace scry::detail::llamad_wire {

// An error whose retry eligibility follows its category, as the turn machine
// applies it, with `token` kept in provider_detail as `llamad:<token>`. The
// message must be a fixed text: nothing the daemon sent may reach it.
[[nodiscard]] Error llamad_error(ErrorCategory category, std::string message,
                                 std::string_view token);

// Writes `request` into `out`: the system prompt as a leading "system" message,
// then the history and the turn's messages. User text becomes one "user"
// message; each tool result becomes its own "tool" message; an assistant
// message carries its text and its tool calls. Every sampling value is sent;
// validation has already bounded them for llamad's float and int32 fields.
// Returns invalid_config for a message shape the wire cannot carry.
[[nodiscard]] Status encode_chat_request(const ModelRequest& request,
                                         ::llamad::v1::ChatRequest& out);

// Decodes one attempt's GenerateChunk stream. Text streams through `sink` as it
// arrives, after one ProviderSemanticOutput; the final chunk's finish reason,
// stats, and tool calls complete the response. The stream shape is the
// daemon's contract: text chunks, then exactly one final chunk with a finish
// reason, and tool calls only there.
class StreamDecoder {
public:
  StreamDecoder(const ResourceLimits& limits, ProviderEventSink& sink) noexcept;

  // Accepts the next chunk. A failure ends the attempt.
  [[nodiscard]] Status consume(const ::llamad::v1::GenerateChunk& chunk);

  // The completed response, once the stream has ended.
  [[nodiscard]] Result<ModelResponse> finish();

private:
  [[nodiscard]] Status account(std::size_t bytes);
  [[nodiscard]] Status report_semantic_output();
  [[nodiscard]] Status publish_text(const std::string& text);
  [[nodiscard]] Status consume_final(const ::llamad::v1::GenerateChunk& chunk);
  [[nodiscard]] Status finish_without_tools(const ::llamad::v1::GenerateChunk& chunk,
                                            FinishReason reason);
  [[nodiscard]] Status finish_with_tools(const ::llamad::v1::GenerateChunk& chunk);

  std::size_t max_response_bytes_;
  std::size_t max_tool_arguments_bytes_;
  ProviderEventSink& sink_;
  ModelResponse response_{};
  std::string text_{};
  std::size_t received_bytes_{};
  bool semantic_output_reported_{false};
  bool final_seen_{false};
};

} // namespace scry::detail::llamad_wire
