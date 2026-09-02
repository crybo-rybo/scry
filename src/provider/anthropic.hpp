/// @file
/// @brief Anthropic Messages API adapter declaration.
///
/// The adapter maps Scry's neutral messages and tools to the streaming
/// Anthropic Messages wire format and decodes its strict event lifecycle back
/// into neutral provider events.

#pragma once

#include "core/provider.hpp"

namespace scry::detail {

/// @brief Stateless strategy for the Anthropic Messages dialect.
///
/// Mutable content-block and message lifecycle state is stored in the supplied
/// `ProviderDecodeState`, so one adapter can serve successive turns safely.
class AnthropicAdapter final : public ProviderAdapter {
public:
  /// @brief Encodes a neutral request for `POST /v1/messages`.
  /// @param config Validated immutable Harness configuration.
  /// @param request Neutral request and captured tool schemas.
  /// @return Streaming HTTP request with Anthropic authentication/version
  ///         headers, or an encoding error.
  [[nodiscard]] Result<TransportRequest>
  make_request(const Config& config, const ModelRequest& request) const override;

  /// @brief Decodes one Anthropic SSE event and advances per-turn state.
  /// @param event_name Anthropic event name.
  /// @param data Complete JSON event payload.
  /// @param state Exclusive decode state for this request stream.
  /// @return Ordered neutral deltas/completion/ignored events, or a strict
  ///         protocol/resource error.
  [[nodiscard]] Result<std::vector<ProviderEvent>>
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state) const override;
};

} // namespace scry::detail
