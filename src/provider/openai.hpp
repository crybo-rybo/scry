#pragma once

#include "core/provider.hpp"

namespace scry::detail {

class OpenAiAdapter final : public ProviderAdapter {
public:
  [[nodiscard]] Result<TransportRequest>
  make_request(const Config& config, const ModelRequest& request) const override;

  [[nodiscard]] Result<ModelResponse>
  parse_response(const TransportResult& result, std::string_view body) const override;

  [[nodiscard]] Result<std::vector<ProviderEvent>>
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state) const override;
};

} // namespace scry::detail
