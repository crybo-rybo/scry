#pragma once

#include "core/backend.hpp"
#include "core/provider.hpp"
#include "core/transport.hpp"

#include <atomic>
#include <memory>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <stop_token>

namespace scry::detail {

// The streaming HTTP backend: the provider adapter encodes the request and
// decodes each server-sent event, the SSE parser frames the response body, and
// the transport moves the bytes. Every supported dialect runs through it.
class HttpStreamBackend final : public ModelBackend {
public:
  HttpStreamBackend(std::unique_ptr<ProviderAdapter> provider,
                    std::unique_ptr<Transport> transport) noexcept;

  [[nodiscard]] Result<ModelResponse>
  perform(const Config& config, const ModelRequest& request, std::stop_token shutdown,
          const std::atomic<bool>& cancelled, ProviderEventSink& sink) override;

private:
  std::unique_ptr<ProviderAdapter> provider_{};
  std::unique_ptr<Transport> transport_{};
};

} // namespace scry::detail
