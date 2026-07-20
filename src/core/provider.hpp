#pragma once

#include "core/model.hpp"
#include "core/transport.hpp"

#include <limits>
#include <map>
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

struct ProviderIgnoredEvent {
  std::string name{};
};

using ProviderEvent =
    std::variant<ProviderTextDelta, ProviderCompleted, ProviderIgnoredEvent>;

struct AnthropicProviderDecodeState {
  std::optional<std::size_t> active_content_index{};
  bool message_started{false};
  bool finish_observed{false};
};

struct OpenAiToolDecodeState {
  std::optional<std::string> id{};
  std::optional<std::string> name{};
  std::optional<std::string> type{};
  std::string arguments{};
};

struct OpenAiProviderDecodeState {
  std::optional<std::string> chunk_id{};
  std::optional<std::size_t> text_content_index{};
  std::map<std::size_t, OpenAiToolDecodeState> tool_calls{};
  bool finish_observed{false};
  bool tools_finalized{false};
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

  [[nodiscard]] virtual Result<std::vector<ProviderEvent>>
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state) const = 0;
};

[[nodiscard]] Result<std::unique_ptr<ProviderAdapter>>
make_provider_adapter(ProviderDialect dialect);

} // namespace scry::detail
