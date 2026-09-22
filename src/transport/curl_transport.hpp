#pragma once

#include "core/transport.hpp"

namespace scry::detail {

class CurlTransport final : public Transport {
public:
  CurlTransport();
  ~CurlTransport() override;

  CurlTransport(const CurlTransport&) = delete;
  CurlTransport& operator=(const CurlTransport&) = delete;

  [[nodiscard]] Status status() const;

  [[nodiscard]] Result<TransportResult> perform(const TransportRequest& request,
                                                std::stop_token shutdown,
                                                const std::atomic<bool>& cancelled,
                                                BodyChunkSink& body_sink) override;

private:
  // libcurl declares CURLM as void, so the handle is held as void* to keep
  // <curl/curl.h> out of this header.
  [[nodiscard]] void* multi();

  void* multi_{};
};

} // namespace scry::detail
