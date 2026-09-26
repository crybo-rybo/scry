#pragma once

#include "core/model.hpp"
#include "core/transport.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <scry/error.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace scry::detail {

struct ProviderTextDelta {
  std::string text{};
};

struct ProviderCompleted {
  ModelResponse response{};
};

// Unknown optional events are ignored without producing an event.
using ProviderEvent = std::variant<ProviderTextDelta, ProviderCompleted>;

struct AnthropicProviderDecodeState {
  std::optional<std::size_t> active_content_index{};
  bool message_started{false};
  bool finish_observed{false};
};

// An empty id or name is rejected on arrival, so empty means not yet seen.
struct OpenAiToolDecodeState {
  std::size_t index{};
  std::string id{};
  std::string name{};
  std::string arguments{};
  bool typed{false};
};

struct OpenAiProviderDecodeState {
  std::string chunk_id{};
  // Sorted by index and stores only observed calls, so an untrusted sparse
  // index cannot size the allocation.
  std::vector<OpenAiToolDecodeState> tool_calls{};
  bool finish_observed{false};
};

using ProviderDialectDecodeState =
    std::variant<std::monostate, AnthropicProviderDecodeState,
                 OpenAiProviderDecodeState>;

struct ProviderDecodeState {
  ModelResponse response{};
  std::size_t max_tool_arguments_bytes{std::numeric_limits<std::size_t>::max()};
  bool semantic_output_consumed{false};
  bool completed{false};
  ProviderDialectDecodeState dialect{};
};

class ProviderAdapter {
public:
  virtual ~ProviderAdapter() = default;

  [[nodiscard]] virtual Result<TransportRequest>
  make_request(const Config& config, const ModelRequest& request) const = 0;

  // Appends this event's decoded events to `out`. The sink is the caller's,
  // so the streaming path keeps one vector for the whole response instead of
  // allocating a fresh one per event; a failed call's partial appends are
  // discarded with the attempt.
  [[nodiscard]] virtual Status
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state,
                     std::vector<ProviderEvent>& out) const = 0;
};

[[nodiscard]] std::unique_ptr<ProviderAdapter>
make_provider_adapter(ProviderDialect dialect);

} // namespace scry::detail
