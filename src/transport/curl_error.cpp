#include "transport/curl_error.hpp"

#include "transport/transport_policy.hpp"

#include <algorithm>
#include <ctime>
#include <curl/curl.h>
#include <limits>
#include <string>
#include <utility>

namespace scry::detail::curl_error {
namespace {

[[nodiscard]] Error make_error(const ErrorCategory category, std::string message,
                               const bool retryable = false) {
  return Error{
      .category = category,
      .message = std::move(message),
      .retryable = retryable,
  };
}

[[nodiscard]] bool is_nonretryable_tls_error(const CURLcode code) noexcept {
  return code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CERTPROBLEM ||
         code == CURLE_SSL_CACERT_BADFILE || code == CURLE_SSL_ISSUER_ERROR;
}

} // namespace

Error cancelled(const AbortCause cause) {
  const auto message = cause == AbortCause::harness_shutdown
                           ? "transfer cancelled by harness shutdown"
                           : "transfer cancelled";
  return make_error(ErrorCategory::cancelled, message);
}

Error classify(const int code_value, const std::optional<Error>& callback_error,
               const AbortCause abort_cause) {
  if (callback_error) {
    return *callback_error;
  }
  if (abort_cause != AbortCause::none) {
    return cancelled(abort_cause);
  }
  const auto code = static_cast<CURLcode>(code_value);
  if (code == CURLE_FILESIZE_EXCEEDED) {
    return make_error(ErrorCategory::resource_limit,
                      "response exceeds configured limit");
  }
  if (is_nonretryable_tls_error(code)) {
    return make_error(ErrorCategory::protocol, "TLS verification failed");
  }
  if (code == CURLE_WEIRD_SERVER_REPLY || code == CURLE_UNSUPPORTED_PROTOCOL ||
      code == CURLE_URL_MALFORMAT) {
    return make_error(ErrorCategory::protocol, "invalid server response");
  }
  return make_error(ErrorCategory::network, "network transfer failed", true);
}

std::optional<std::chrono::milliseconds>
retry_after(const std::vector<HttpHeader>& headers) {
  for (const auto& header : headers) {
    if (!transport_policy::header_name_equal(header.name, "retry-after")) {
      continue;
    }
    if (const auto seconds = transport_policy::parse_size(header.value)) {
      constexpr auto maximum =
          std::numeric_limits<std::chrono::milliseconds::rep>::max();
      const auto bounded = std::min(*seconds, static_cast<std::size_t>(maximum / 1000));
      return std::chrono::milliseconds{
          static_cast<std::chrono::milliseconds::rep>(bounded * 1000)};
    }
    const auto parsed_date = curl_getdate(header.value.c_str(), nullptr);
    const auto current_time = std::time(nullptr);
    if (parsed_date >= 0 && current_time >= 0) {
      const auto seconds = std::max(parsed_date - current_time, std::time_t{0});
      return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::seconds{seconds});
    }
  }
  return std::nullopt;
}

} // namespace scry::detail::curl_error
