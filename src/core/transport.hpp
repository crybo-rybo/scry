#pragma once

#include <atomic>
#include <cstdint>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/unique_function.hpp>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scry::detail {

struct HttpHeader {
  std::string name{};
  std::string value{};
};

struct TransportRequest {
  std::string url{};
  std::vector<HttpHeader> headers{};
  std::string body{};
  bool tls_verify_peer{true};
  TransportTimeouts timeouts{};
  ResourceLimits limits{};
};

struct TransportResult {
  std::int32_t status_code{};
  std::vector<HttpHeader> headers{};
  std::string provider_request_id{};
};

using BodyChunkSink = UniqueFunction<Status(std::string_view)>;

class Transport {
public:
  virtual ~Transport() = default;

  [[nodiscard]] virtual Result<TransportResult>
  perform(const TransportRequest& request, std::stop_token shutdown,
          const std::atomic<bool>& cancelled, BodyChunkSink& body_sink) = 0;
};

} // namespace scry::detail
