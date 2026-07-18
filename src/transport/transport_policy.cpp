#include "transport/transport_policy.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <string>
#include <utility>

namespace scry::detail::transport_policy {
namespace {

[[nodiscard]] Error make_error(const ErrorCategory category, std::string message,
                               const bool retryable = false) {
  return Error{
      .category = category,
      .message = std::move(message),
      .retryable = retryable,
  };
}

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

} // namespace

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

bool is_content_length_header(const std::string_view name) noexcept {
  return header_name_equal(name, "content-length");
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
  if (request.timeouts.connect <= std::chrono::milliseconds::zero() ||
      request.timeouts.transfer <= std::chrono::milliseconds::zero()) {
    return std::unexpected(make_error(ErrorCategory::invalid_config,
                                      "transport timeouts must be positive"));
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

} // namespace scry::detail::transport_policy
