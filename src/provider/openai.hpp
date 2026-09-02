/// @file
/// @brief OpenAI-compatible Chat Completions adapter declaration.
///
/// This strategy implements Scry's documented common streaming subset for
/// OpenAI, vLLM, Ollama, llama.cpp server, and LM Studio—not every vendor
/// extension or legacy API shape.

#pragma once

#include "core/provider.hpp"

namespace scry::detail {

/// @brief Stateless strategy for the OpenAI-compatible dialect.
///
/// Indexed tool fragments, completion IDs, finish state, and usage live in the
/// per-request `ProviderDecodeState`; they never become adapter instance state.
class OpenAiAdapter final : public ProviderAdapter {
public:
  /// @brief Encodes a neutral request for the Chat Completions endpoint.
  /// @param config Validated immutable Harness configuration.
  /// @param request Neutral request and captured tool schemas.
  /// @return Streaming HTTP request with optional Bearer authorization, or an
  ///         encoding/configuration error.
  [[nodiscard]] Result<TransportRequest>
  make_request(const Config& config, const ModelRequest& request) const override;

  /// @brief Decodes one OpenAI-compatible SSE event and advances turn state.
  /// @param event_name SSE event name; normally `message`.
  /// @param data JSON chunk or the terminal `[DONE]` marker.
  /// @param state Exclusive decode state for this request stream.
  /// @return Ordered neutral events, or an error for malformed lifecycle,
  ///         required data, or resource-limit violations.
  [[nodiscard]] Result<std::vector<ProviderEvent>>
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state) const override;
};

} // namespace scry::detail
