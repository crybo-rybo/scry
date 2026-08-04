#pragma once

#include "core/transport.hpp"

#include <memory>

namespace scry::detail {

class CurlTransport final : public Transport {
public:
  CurlTransport();
  ~CurlTransport() override;

  CurlTransport(const CurlTransport&) = delete;
  CurlTransport& operator=(const CurlTransport&) = delete;
  CurlTransport(CurlTransport&&) = delete;
  CurlTransport& operator=(CurlTransport&&) = delete;

  [[nodiscard]] Status status() const;

  [[nodiscard]] Result<TransportResult> perform(const TransportRequest& request,
                                                std::stop_token shutdown,
                                                const std::atomic<bool>& cancelled,
                                                BodyChunkSink& body_sink) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace scry::detail
