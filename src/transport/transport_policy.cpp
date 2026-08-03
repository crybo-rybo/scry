#include "transport/transport_policy.hpp"

#include "core/json_codec.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <utility>

namespace scry::detail::transport_policy {
namespace {

[[nodiscard]] bool valid_header_name(const std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  return std::ranges::all_of(name, [](const char character) {
    const auto value = static_cast<unsigned char>(character);
    constexpr std::string_view punctuation{"!#$%&'*+-.^_`|~"};
    return std::isalnum(value) != 0 ||
           punctuation.find(character) != std::string_view::npos;
  });
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] Result<std::int32_t> response_status(const std::string_view line) {
  const auto first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "malformed HTTP status line"));
  }
  auto value = line.substr(first_space + 1);
  value = value.substr(0, value.find(' '));
  std::int32_t status = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), status);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "malformed HTTP status line"));
  }
  return status;
}

[[nodiscard]] Status account_bytes(ResponseState& response, const std::size_t bytes) {
  if (response.received_bytes > response.limit ||
      bytes > response.limit - response.received_bytes) {
    return std::unexpected(
        make_error(ErrorCategory::resource_limit, "response exceeds configured limit"));
  }
  response.received_bytes += bytes;
  return {};
}

[[nodiscard]] Status accept_status_line(ResponseState& response,
                                        const std::string_view line) {
  auto status = response_status(line);
  if (!status) {
    return std::unexpected(std::move(status.error()));
  }
  response.deliver_body = *status >= 200 && *status < 300;
  response.headers.clear();
  response.provider_request_id.clear();
  return {};
}

[[nodiscard]] Status record_header(ResponseState& response, const std::string_view name,
                                   const std::string_view value) {
  response.headers.push_back(
      HttpHeader{.name = std::string{name}, .value = std::string{value}});
  if (!is_request_id_header(name)) {
    return {};
  }
  constexpr std::size_t maximum_request_id_bytes = 256;
  if (value.size() > maximum_request_id_bytes) {
    return std::unexpected(make_error(ErrorCategory::protocol,
                                      "provider request identifier is too large"));
  }
  response.provider_request_id = value;
  return {};
}

} // namespace

Status ResponseState::accept_header(std::string_view line) {
  if (auto status = account_bytes(*this, line.size()); !status) {
    return status;
  }
  line = trim(line);
  if (line.empty()) {
    return {};
  }
  if (line.starts_with("HTTP/")) {
    return accept_status_line(*this, line);
  }
  const auto separator = line.find(':');
  if (separator == std::string_view::npos) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "malformed response header"));
  }
  const auto name = trim(line.substr(0, separator));
  const auto value = trim(line.substr(separator + 1));
  if (name.empty()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "malformed response header"));
  }
  return record_header(*this, name, value);
}

Status ResponseState::account_body(const std::size_t bytes) {
  return account_bytes(*this, bytes);
}

bool header_name_equal(const std::string_view left,
                       const std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  return std::ranges::equal(left, right, [](const char lhs, const char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
  });
}

bool is_request_id_header(const std::string_view name) noexcept {
  return header_name_equal(name, "request-id") ||
         header_name_equal(name, "x-request-id") ||
         header_name_equal(name, "anthropic-request-id");
}

std::optional<std::size_t> parse_size(const std::string_view value) noexcept {
  std::size_t parsed{};
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

Status validate_request(const TransportRequest& request,
                        const BodyChunkSink& body_sink) {
  if (request.url.empty()) {
    return std::unexpected(
        make_error(ErrorCategory::invalid_config, "transport URL is empty"));
  }
  if (!body_sink) {
    return std::unexpected(
        make_error(ErrorCategory::invalid_state, "response sink is missing"));
  }
  return {};
}

Status validate_headers(const std::vector<HttpHeader>& headers) {
  for (const auto& header : headers) {
    if (!valid_header_name(header.name) ||
        header.value.find_first_of("\r\n") != std::string::npos) {
      return std::unexpected(
          make_error(ErrorCategory::protocol, "invalid request header"));
    }
  }
  return {};
}

Error http_error(const std::int32_t status, const std::string& request_id) {
  Error error;
  if (status == 401 || status == 403) {
    error = make_error(ErrorCategory::authentication, "provider authentication failed");
  } else if (status == 429) {
    error = make_error(ErrorCategory::rate_limit, "provider rate limit exceeded", true);
  } else if (status >= 500 && status <= 599) {
    error = make_error(ErrorCategory::network, "provider server error", true);
  } else {
    error = make_error(ErrorCategory::protocol, "provider rejected the request");
  }
  error.provider_request_id = request_id;
  return error;
}

std::string sanitize_provider_detail(const std::string_view detail) {
  constexpr std::size_t maximum_size = 128;
  const auto separator = detail.find(':');
  if (detail.empty() || detail.size() > maximum_size ||
      separator == std::string_view::npos || separator == 0 ||
      separator + 1 == detail.size() ||
      detail.find(':', separator + 1) != std::string_view::npos) {
    return {};
  }
  const auto safe = std::ranges::all_of(detail, [](const char value) {
    const auto character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || value == '_' || value == ':';
  });
  return safe ? std::string{detail} : std::string{};
}

} // namespace scry::detail::transport_policy
