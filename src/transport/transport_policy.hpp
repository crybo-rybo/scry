#pragma once

#include "core/transport.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace scry::detail::transport_policy {

[[nodiscard]] bool header_name_equal(std::string_view left,
                                     std::string_view right) noexcept;

[[nodiscard]] bool is_request_id_header(std::string_view name) noexcept;

[[nodiscard]] bool is_content_length_header(std::string_view name) noexcept;

[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view value) noexcept;

[[nodiscard]] Status validate_request(const TransportRequest& request,
                                      const BodyChunkSink& body_sink);

[[nodiscard]] Status validate_headers(const std::vector<HttpHeader>& headers);

[[nodiscard]] Error http_error(std::int32_t status, const std::string& request_id);

} // namespace scry::detail::transport_policy
