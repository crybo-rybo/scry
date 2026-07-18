#pragma once

#include "core/model.hpp"
#include "core/transport.hpp"

#include <memory>
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

struct ProviderDecodeState {
  ModelResponse response{};
  bool semantic_output_consumed{false};
  bool completed{false};
};

class ProviderAdapter {
public:
  virtual ~ProviderAdapter() = default;

  [[nodiscard]] virtual Result<TransportRequest>
  make_request(const Config& config, const ModelRequest& request) const = 0;

  [[nodiscard]] virtual Result<ModelResponse>
  parse_response(const TransportResult& result, std::string_view body) const = 0;

  [[nodiscard]] virtual Result<std::vector<ProviderEvent>>
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state) const = 0;
};

[[nodiscard]] Result<std::unique_ptr<ProviderAdapter>>
make_provider_adapter(ProviderDialect dialect);

} // namespace scry::detail
