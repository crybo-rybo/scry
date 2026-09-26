#pragma once

#include "core/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scry::detail::transport_policy {

// At most this much of a non-2xx body is retained, and only to extract the
// provider's error token.
inline constexpr std::size_t maximum_error_body_bytes = std::size_t{8} * 1024;

struct ResponseState {
  std::size_t limit{};
  std::size_t received_bytes{};
  // Only the request id and Retry-After values are retained; every other header
  // is dropped as it arrives. Retry-After values are kept in arrival order
  // because the first that parses wins, and are reset with the fields below on a
  // new status line.
  std::vector<std::string> retry_after_values{};
  std::string provider_request_id{};
  // Status of the most recent status line, reset with the other per-response
  // fields whenever a new one arrives. Zero until the first status line.
  std::int32_t status_code{};
  bool deliver_body{false};

  [[nodiscard]] Status accept_header(std::string_view line);
  [[nodiscard]] Status account_body(std::size_t bytes);
};

[[nodiscard]] bool header_name_equal(std::string_view left,
                                     std::string_view right) noexcept;

[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view value) noexcept;

[[nodiscard]] Status validate_timeouts(const TransportTimeouts& timeouts);

[[nodiscard]] Status validate_request(const TransportRequest& request,
                                      const BodyChunkSink& body_sink);

[[nodiscard]] Status validate_headers(const std::vector<HttpHeader>& headers);

// Appends `chunk` to a retained error body without growing it past
// maximum_error_body_bytes.
void append_error_body(std::string& body, std::string_view chunk);

[[nodiscard]] Error http_error(std::int32_t status, const std::string& request_id);

// http_error plus the provider token mined from the retained error body.
[[nodiscard]] Error http_error(std::int32_t status, const std::string& request_id,
                               std::string_view body,
                               std::string_view provider_namespace);

// Extracts the provider's own error token from a non-2xx response body. The
// body itself, the provider message, and every other field are discarded.
[[nodiscard]] std::string http_error_detail(std::string_view body,
                                            std::string_view provider_namespace);

[[nodiscard]] std::string sanitize_provider_detail(std::string_view detail);

} // namespace scry::detail::transport_policy
