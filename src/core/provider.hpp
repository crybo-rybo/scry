/// @file
/// @brief Internal provider-adapter seam and per-turn streaming decode state.
///
/// Adapters translate between provider-neutral model values and dialect wire
/// formats. Adapter objects remain stateless; all mutable stream lifecycle and
/// fragment accumulation lives in `ProviderDecodeState`, owned by one turn.

#pragma once

#include "core/model.hpp"
#include "core/transport.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <scry/error.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scry::detail {

/// @brief Incremental provider event containing newly decoded assistant text.
struct ProviderTextDelta {
  std::string text{}; ///< Owning UTF-8 bytes decoded from one stream event.
};

/// @brief Terminal provider event containing the accumulated neutral response.
struct ProviderCompleted {
  ModelResponse response{}; ///< Complete response moved out of decode state.
};

/// @brief Debug-observable optional wire event that has no neutral mapping.
///
/// Required unknown content is a protocol error instead. This event is used
/// only for provider features that can safely be ignored without changing the
/// turn's meaning.
struct ProviderIgnoredEvent {
  std::string name{}; ///< Bounded event/type name useful for diagnostics.
};

/// @brief Closed set of neutral events emitted by a provider decoder.
using ProviderEvent =
    std::variant<ProviderTextDelta, ProviderCompleted, ProviderIgnoredEvent>;

/// @brief Mutable lifecycle state for one Anthropic Messages stream.
struct AnthropicProviderDecodeState {
  std::optional<std::size_t> active_content_index{}; ///< Open content block index.
  bool message_started{false}; ///< Whether the unique `message_start` arrived.
  bool finish_observed{false}; ///< Whether `message_delta` supplied a stop reason.
};

/// @brief Accumulator for one indexed OpenAI streamed tool call.
///
/// Tool calls are stored sparsely in a sorted vector rather than indexed
/// directly, preventing a hostile numeric index from controlling allocation.
struct OpenAiToolDecodeState {
  static constexpr std::uint8_t id_present = 1U << 0U;   ///< ID metadata seen.
  static constexpr std::uint8_t name_present = 1U << 1U; ///< Function name seen.
  static constexpr std::uint8_t type_present = 1U << 2U; ///< `function` type seen.

  std::size_t index{};     ///< Provider-assigned position in the call array.
  std::string id{};        ///< Accumulated stable call identifier.
  std::string name{};      ///< Accumulated function name.
  std::string arguments{}; ///< Concatenated JSON argument fragments.
  std::uint8_t metadata{}; ///< Bit set recording which required metadata arrived.
};

/// @brief Mutable lifecycle state for one OpenAI-compatible completion stream.
struct OpenAiProviderDecodeState {
  /// Sentinel meaning no neutral text block has yet been created.
  static constexpr std::size_t no_text_content =
      std::numeric_limits<std::size_t>::max();

  std::string chunk_id{}; ///< Stable completion ID required across all chunks.
  std::size_t text_content_index{no_text_content}; ///< Response text-block index.
  std::vector<OpenAiToolDecodeState> tool_calls{}; ///< Sparse calls sorted by index.
  bool finish_observed{false}; ///< Whether a choice finish reason arrived.
  bool tools_finalized{false}; ///< Whether fragments were validated and materialized.
};

/// @brief Dialect-specific alternative owned by a shared decode context.
///
/// `monostate` is claimed lazily by the first adapter event. Reusing state with
/// a different dialect is rejected as a protocol error.
using ProviderDialectDecodeState =
    std::variant<std::monostate, AnthropicProviderDecodeState,
                 OpenAiProviderDecodeState>;

/// @brief Complete mutable decoding context for one model request stream.
/// @invariant Once `completed` is true, every subsequent wire event is rejected.
struct ProviderDecodeState {
  ModelResponse response{}; ///< Neutral response accumulated across stream events.
  std::size_t max_tool_arguments_bytes{std::numeric_limits<std::size_t>::max()};
  ///< Per-call cap checked before appending argument fragments.
  bool semantic_output_consumed{false}; ///< Disables automatic retry once set.
  bool completed{false}; ///< Terminal response has been emitted and moved out.
  ProviderDialectDecodeState dialect{}; ///< Per-dialect lifecycle state.
};

/// @brief Strategy interface for one provider wire dialect.
///
/// Implementations are immutable translators. They may be shared by sequential
/// turns because every mutable parse field is supplied through
/// `ProviderDecodeState`. The runtime owns adapter lifetime.
class ProviderAdapter {
public:
  /// @brief Enables destruction through the internal strategy interface.
  virtual ~ProviderAdapter() = default;

  /// @brief Encodes one neutral request as a bounded HTTP request.
  /// @param config Validated Harness configuration for this dialect.
  /// @param request Provider-neutral request snapshot.
  /// @return An owning transport request, or an encoding/configuration error.
  [[nodiscard]] virtual Result<TransportRequest>
  make_request(const Config& config, const ModelRequest& request) const = 0;

  /// @brief Applies one decoded SSE event to a per-turn stream state.
  /// @param event_name SSE event name; borrowed for this call only.
  /// @param data Complete SSE data payload; borrowed for this call only.
  /// @param state Mutable state belonging exclusively to this request stream.
  /// @return Zero or more ordered neutral events, or a protocol/resource error.
  /// @note Required unknown content fails; safely optional events may produce
  ///       `ProviderIgnoredEvent`.
  [[nodiscard]] virtual Result<std::vector<ProviderEvent>>
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state) const = 0;
};

/// @brief Creates the stateless adapter selected by Harness configuration.
/// @param dialect Supported provider dialect.
/// @return Exclusive ownership of the corresponding adapter implementation.
[[nodiscard]] std::unique_ptr<ProviderAdapter>
make_provider_adapter(ProviderDialect dialect);

} // namespace scry::detail
