#include "transport/curl_transport.hpp"

#include "transport/curl_global.hpp"
#include "transport/transport_policy.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace scry::detail {
namespace {

[[nodiscard]] Error make_error(const ErrorCategory category, std::string message,
                               const bool retryable = false) {
  return Error{
      .category = category,
      .message = std::move(message),
      .retryable = retryable,
  };
}

struct EasyDeleter {
  void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
};

using EasyHandle = std::unique_ptr<CURL, EasyDeleter>;

class HeaderList final {
public:
  HeaderList() = default;

  ~HeaderList() {
    if (headers_ != nullptr) {
      curl_slist_free_all(headers_);
    }
  }

  HeaderList(HeaderList&& other) noexcept
      : headers_(std::exchange(other.headers_, nullptr)) {}

  HeaderList(const HeaderList&) = delete;
  HeaderList& operator=(const HeaderList&) = delete;
  HeaderList& operator=(HeaderList&&) = delete;

  [[nodiscard]] bool append(const std::string& header) noexcept {
    auto* appended = curl_slist_append(headers_, header.c_str());
    if (appended == nullptr) {
      return false;
    }
    headers_ = appended;
    return true;
  }

  [[nodiscard]] curl_slist* get() const noexcept { return headers_; }

private:
  curl_slist* headers_{};
};

enum class AbortCause : std::uint8_t {
  none,
  turn_cancelled,
  harness_shutdown,
};

struct TransferContext {
  std::stop_token shutdown{};
  const std::atomic<bool>* cancelled{};
  BodyChunkSink* body_sink{};
  std::size_t response_limit{};
  std::size_t received_bytes{};
  std::vector<HttpHeader> headers{};
  std::string provider_request_id{};
  std::optional<Error> callback_error{};
  AbortCause abort_cause{AbortCause::none};
};

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

void set_callback_error(TransferContext& context, Error error) noexcept {
  context.callback_error = std::move(error);
}

[[nodiscard]] Status accept_header(TransferContext& context, std::string_view line) {
  line = trim(line);
  if (line.empty()) {
    return {};
  }
  if (line.starts_with("HTTP/")) {
    context.headers.clear();
    context.provider_request_id.clear();
    return {};
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
  if (transport_policy::is_content_length_header(name)) {
    const auto length = transport_policy::parse_size(value);
    if (!length) {
      return std::unexpected(
          make_error(ErrorCategory::protocol, "invalid response length"));
    }
    if (*length > context.response_limit) {
      return std::unexpected(make_error(ErrorCategory::resource_limit,
                                        "response exceeds configured limit"));
    }
  }
  context.headers.push_back(
      HttpHeader{.name = std::string{name}, .value = std::string{value}});
  if (transport_policy::is_request_id_header(name)) {
    context.provider_request_id = value;
  }
  return {};
}

std::size_t header_callback(char* data, const std::size_t size, const std::size_t count,
                            void* userdata) noexcept {
  auto& context = *static_cast<TransferContext*>(userdata);
  const auto bytes = size * count;
  try {
    auto status =
        accept_header(context, std::string_view{data, static_cast<std::size_t>(bytes)});
    if (!status) {
      set_callback_error(context, std::move(status.error()));
      return 0;
    }
    return bytes;
  } catch (...) {
    set_callback_error(context, make_error(ErrorCategory::protocol,
                                           "response header processing failed"));
    return 0;
  }
}

[[nodiscard]] Status accept_body(TransferContext& context,
                                 const std::string_view chunk) {
  if (context.received_bytes > context.response_limit ||
      chunk.size() > context.response_limit - context.received_bytes) {
    return std::unexpected(
        make_error(ErrorCategory::resource_limit, "response exceeds configured limit"));
  }
  context.received_bytes += chunk.size();
  auto status = (*context.body_sink)(chunk);
  if (!status) {
    return std::unexpected(Error{
        .category = status.error().category,
        .message = "response consumer rejected response data",
        .retryable = status.error().retryable,
        .turn_id = status.error().turn_id,
        .attempt = status.error().attempt,
        .provider_request_id = status.error().provider_request_id,
    });
  }
  return {};
}

std::size_t body_callback(char* data, const std::size_t size, const std::size_t count,
                          void* userdata) noexcept {
  auto& context = *static_cast<TransferContext*>(userdata);
  const auto bytes = size * count;
  try {
    auto status =
        accept_body(context, std::string_view{data, static_cast<std::size_t>(bytes)});
    if (!status) {
      set_callback_error(context, std::move(status.error()));
      return 0;
    }
    return bytes;
  } catch (...) {
    set_callback_error(context, make_error(ErrorCategory::protocol,
                                           "response body processing failed"));
    return 0;
  }
}

int progress_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t,
                      curl_off_t) noexcept {
  auto& context = *static_cast<TransferContext*>(userdata);
  if (context.shutdown.stop_requested()) {
    context.abort_cause = AbortCause::harness_shutdown;
    return 1;
  }
  if (context.cancelled->load(std::memory_order_acquire)) {
    context.abort_cause = AbortCause::turn_cancelled;
    return 1;
  }
  return 0;
}

[[nodiscard]] Result<HeaderList> build_headers(const std::vector<HttpHeader>& headers) {
  if (auto status = transport_policy::validate_headers(headers); !status) {
    return std::unexpected(std::move(status.error()));
  }
  HeaderList list;
  for (const auto& header : headers) {
    if (!list.append(header.name + ": " + header.value)) {
      return std::unexpected(make_error(ErrorCategory::resource_limit,
                                        "request header allocation failed"));
    }
  }
  return list;
}

[[nodiscard]] long
timeout_milliseconds(const std::chrono::milliseconds value) noexcept {
  return static_cast<long>(std::min<std::chrono::milliseconds::rep>(
      value.count(), static_cast<std::chrono::milliseconds::rep>(LONG_MAX)));
}

[[nodiscard]] Status check_setopt(const CURLcode code) {
  if (code == CURLE_OK) {
    return {};
  }
  return std::unexpected(
      make_error(ErrorCategory::protocol, "libcurl request setup failed"));
}

[[nodiscard]] Status configure_callbacks(CURL* easy, TransferContext& context) {
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, body_callback));
      !status) {
    return status;
  }
  if (auto status = check_setopt(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &context));
      !status) {
    return status;
  }
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_callback));
      !status) {
    return status;
  }
  if (auto status = check_setopt(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &context));
      !status) {
    return status;
  }
  if (auto status = check_setopt(
          curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress_callback));
      !status) {
    return status;
  }
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &context));
      !status) {
    return status;
  }
  return check_setopt(curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L));
}

[[nodiscard]] Status configure_timeouts(CURL* easy, const TransportRequest& request) {
  if (auto status = check_setopt(
          curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                           timeout_milliseconds(request.timeouts.connect)));
      !status) {
    return status;
  }
  return check_setopt(curl_easy_setopt(
      easy, CURLOPT_TIMEOUT_MS, timeout_milliseconds(request.timeouts.transfer)));
}

[[nodiscard]] Status configure_tls(CURL* easy, const TransportRequest& request) {
  const auto verify_peer = request.tls_verify_peer ? 1L : 0L;
  const auto verify_host = request.tls_verify_peer ? 2L : 0L;
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, verify_peer));
      !status) {
    return status;
  }
  return check_setopt(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, verify_host));
}

[[nodiscard]] Status configure_request(CURL* easy, const TransportRequest& request,
                                       HeaderList& headers) {
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str()));
      !status) {
    return status;
  }
  if (auto status = check_setopt(curl_easy_setopt(easy, CURLOPT_POST, 1L)); !status) {
    return status;
  }
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.data()));
      !status) {
    return status;
  }
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                                        static_cast<curl_off_t>(request.body.size())));
      !status) {
    return status;
  }
  if (auto status =
          check_setopt(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.get()));
      !status) {
    return status;
  }
  if (auto status = check_setopt(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
      !status) {
    return status;
  }
  return check_setopt(
      curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE,
                       static_cast<curl_off_t>(request.limits.max_response_bytes)));
}

[[nodiscard]] Error cancelled_error(const AbortCause cause) {
  const auto message = cause == AbortCause::harness_shutdown
                           ? "transfer cancelled by harness shutdown"
                           : "transfer cancelled";
  return make_error(ErrorCategory::cancelled, message);
}

[[nodiscard]] bool is_nonretryable_tls_error(const CURLcode code) noexcept {
  switch (code) {
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CACERT_BADFILE:
  case CURLE_SSL_ISSUER_ERROR:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] Error curl_error(const CURLcode code, const TransferContext& context) {
  if (context.callback_error) {
    return *context.callback_error;
  }
  if (context.abort_cause != AbortCause::none) {
    return cancelled_error(context.abort_cause);
  }
  if (code == CURLE_FILESIZE_EXCEEDED) {
    return make_error(ErrorCategory::resource_limit,
                      "response exceeds configured limit");
  }
  if (is_nonretryable_tls_error(code)) {
    return make_error(ErrorCategory::network, "TLS verification failed");
  }
  if (code == CURLE_WEIRD_SERVER_REPLY || code == CURLE_UNSUPPORTED_PROTOCOL ||
      code == CURLE_URL_MALFORMAT) {
    return make_error(ErrorCategory::protocol, "invalid server response");
  }
  return make_error(ErrorCategory::network, "network transfer failed", true);
}

[[nodiscard]] Result<TransportResult> finish_transfer(CURL* easy, const CURLcode code,
                                                      TransferContext& context) {
  if (code != CURLE_OK) {
    auto error = curl_error(code, context);
    error.provider_request_id = context.provider_request_id;
    return std::unexpected(std::move(error));
  }
  long response_code{};
  if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code) != CURLE_OK ||
      response_code > std::numeric_limits<std::int32_t>::max()) {
    return std::unexpected(
        make_error(ErrorCategory::protocol, "missing HTTP response status"));
  }
  const auto status = static_cast<std::int32_t>(response_code);
  if (status < 200 || status >= 300) {
    return std::unexpected(
        transport_policy::http_error(status, context.provider_request_id));
  }
  return TransportResult{
      .status_code = status,
      .headers = std::move(context.headers),
      .provider_request_id = std::move(context.provider_request_id),
  };
}

} // namespace

class CurlTransport::Impl final {
public:
  Impl() = default;

  [[nodiscard]] const std::optional<Error>& startup_error() const noexcept {
    return global_.error();
  }

private:
  CurlGlobalLease global_;
};

CurlTransport::CurlTransport() : impl_(std::make_unique<Impl>()) {}

CurlTransport::~CurlTransport() = default;

CurlTransport::CurlTransport(CurlTransport&&) noexcept = default;

CurlTransport& CurlTransport::operator=(CurlTransport&&) noexcept = default;

Result<TransportResult> CurlTransport::perform(const TransportRequest& request,
                                               const std::stop_token shutdown,
                                               const std::atomic<bool>& cancelled,
                                               BodyChunkSink& body_sink) {
  if (impl_->startup_error()) {
    return std::unexpected(*impl_->startup_error());
  }
  if (shutdown.stop_requested()) {
    return std::unexpected(cancelled_error(AbortCause::harness_shutdown));
  }
  if (cancelled.load(std::memory_order_acquire)) {
    return std::unexpected(cancelled_error(AbortCause::turn_cancelled));
  }
  if (auto status = transport_policy::validate_request(request, body_sink); !status) {
    return std::unexpected(std::move(status.error()));
  }
  auto headers = build_headers(request.headers);
  if (!headers) {
    return std::unexpected(std::move(headers.error()));
  }
  EasyHandle easy{curl_easy_init()};
  if (!easy) {
    return std::unexpected(
        make_error(ErrorCategory::network, "libcurl request initialization failed"));
  }
  TransferContext context{
      .shutdown = shutdown,
      .cancelled = &cancelled,
      .body_sink = &body_sink,
      .response_limit = request.limits.max_response_bytes,
  };
  if (auto status = configure_request(easy.get(), request, *headers); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (auto status = configure_callbacks(easy.get(), context); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (auto status = configure_timeouts(easy.get(), request); !status) {
    return std::unexpected(std::move(status.error()));
  }
  if (auto status = configure_tls(easy.get(), request); !status) {
    return std::unexpected(std::move(status.error()));
  }
  const auto code = curl_easy_perform(easy.get());
  return finish_transfer(easy.get(), code, context);
}

} // namespace scry::detail
