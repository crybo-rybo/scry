#pragma once

#include "core/provider.hpp"

namespace scry::detail {

class OpenAiAdapter final : public ProviderAdapter {
public:
  [[nodiscard]] Result<TransportRequest>
  make_request(const Config& config, const ModelRequest& request) const override;

  [[nodiscard]] Status
  parse_stream_event(std::string_view event_name, std::string_view data,
                     ProviderDecodeState& state,
                     std::vector<ProviderEvent>& out) const override;
};

} // namespace scry::detail
