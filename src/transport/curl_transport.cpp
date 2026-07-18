#include "transport/curl_transport.hpp"

#include "transport/curl_error.hpp"
#include "transport/curl_global.hpp"
#include "transport/transport_policy.hpp"

#include <algorithm>
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

class MultiTransfer final {
public:
  MultiTransfer() : multi_(curl_multi_init()) {}

  ~MultiTransfer() {
    if (multi_ != nullptr && easy_ != nullptr) {
      static_cast<void>(curl_multi_remove_handle(multi_, easy_));
    }
    if (multi_ != nullptr) {
      static_cast<void>(curl_multi_cleanup(multi_));
    }
  }

  MultiTransfer(const MultiTransfer&) = delete;
  MultiTransfer& operator=(const MultiTransfer&) = delete;

  [[nodiscard]] bool valid() const noexcept { return multi_ != nullptr; }

  [[nodiscard]] CURLMcode add(CURL* easy) noexcept {
    const auto code = curl_multi_add_handle(multi_, easy);
    if (code == CURLM_OK) {
      easy_ = easy;
    }
    return code;
  }

  [[nodiscard]] CURLM* get() const noexcept { return multi_; }

private:
  CURLM* multi_{};
  CURL* easy_{};
};

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

struct TransferContext {
  std::stop_token shutdown{};
  const std::atomic<bool>* cancelled{};
  BodyChunkSink* body_sink{};
  transport_policy::ResponseState response{};
  std::optional<Error> callback_error{};
  curl_error::AbortCause abort_cause{curl_error::AbortCause::none};
};

void set_callback_error(TransferContext& context, Error error) noexcept {
  context.callback_error = std::move(error);
}

std::size_t header_callback(char* data, const std::size_t size, const std::size_t count,
                            void* userdata) noexcept {
  auto& context = *static_cast<TransferContext*>(userdata);
  const auto bytes = size * count;
  try {
    auto status = context.response.accept_header(
        std::string_view{data, static_cast<std::size_t>(bytes)});
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
  if (auto status = context.response.account_body(chunk.size()); !status) {
    return status;
  }
  if (!context.response.deliver_body) {
    return {};
  }
  auto status = (*context.body_sink)(chunk);
  if (!status) {
    return std::unexpected(Error{
        .category = status.error().category,
        .message = "response consumer rejected response data",
        .provider_detail =
            transport_policy::sanitize_provider_detail(status.error().provider_detail),
        .retryable = status.error().retryable,
        .retry_after = status.error().retry_after,
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
    context.abort_cause = curl_error::AbortCause::harness_shutdown;
    return 1;
  }
  if (context.cancelled->load(std::memory_order_acquire)) {
    context.abort_cause = curl_error::AbortCause::turn_cancelled;
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

[[nodiscard]] int
poll_timeout_milliseconds(const std::chrono::milliseconds value) noexcept {
  return static_cast<int>(std::min<std::chrono::milliseconds::rep>(
      value.count(), static_cast<std::chrono::milliseconds::rep>(INT_MAX)));
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

[[nodiscard]] Status configure_easy(CURL* easy, const TransportRequest& request,
                                    HeaderList& headers, TransferContext& context) {
  if (auto status = configure_request(easy, request, headers); !status) {
    return status;
  }
  if (auto status = configure_callbacks(easy, context); !status) {
    return status;
  }
  if (auto status = configure_timeouts(easy, request); !status) {
    return status;
  }
  return configure_tls(easy, request);
}

[[nodiscard]] Status validate_execution(const std::optional<Error>& startup_error,
                                        const std::stop_token& shutdown,
                                        const std::atomic<bool>& cancelled) {
  if (startup_error) {
    return std::unexpected(*startup_error);
  }
  if (shutdown.stop_requested()) {
    return std::unexpected(
        curl_error::cancelled(curl_error::AbortCause::harness_shutdown));
  }
  if (cancelled.load(std::memory_order_acquire)) {
    return std::unexpected(
        curl_error::cancelled(curl_error::AbortCause::turn_cancelled));
  }
  return {};
}

[[nodiscard]] bool cancellation_requested(TransferContext& context) noexcept {
  if (context.shutdown.stop_requested()) {
    context.abort_cause = curl_error::AbortCause::harness_shutdown;
    return true;
  }
  if (context.cancelled->load(std::memory_order_acquire)) {
    context.abort_cause = curl_error::AbortCause::turn_cancelled;
    return true;
  }
  return false;
}

[[nodiscard]] Result<CURLcode>
drive_transfer(MultiTransfer& multi, TransferContext& context,
               const std::chrono::milliseconds shutdown_bound) {
  int running = 0;
  while (true) {
    if (cancellation_requested(context)) {
      return CURLE_ABORTED_BY_CALLBACK;
    }
    if (curl_multi_perform(multi.get(), &running) != CURLM_OK) {
      return std::unexpected(
          make_error(ErrorCategory::network, "libcurl transfer driver failed", true));
    }
    if (running == 0) {
      break;
    }
    int ready = 0;
    if (curl_multi_poll(multi.get(), nullptr, 0,
                        poll_timeout_milliseconds(shutdown_bound),
                        &ready) != CURLM_OK) {
      return std::unexpected(
          make_error(ErrorCategory::network, "libcurl transfer wait failed", true));
    }
  }

  int messages_remaining = 0;
  while (auto* message = curl_multi_info_read(multi.get(), &messages_remaining)) {
    if (message->msg == CURLMSG_DONE) {
      return message->data.result;
    }
  }
  return std::unexpected(
      make_error(ErrorCategory::protocol, "libcurl transfer result is missing"));
}

[[nodiscard]] Result<TransportResult> finish_transfer(CURL* easy, const CURLcode code,
                                                      TransferContext& context) {
  if (code != CURLE_OK && code != CURLE_HTTP_RETURNED_ERROR) {
    auto error = curl_error::classify(static_cast<int>(code), context.callback_error,
                                      context.abort_cause);
    if (error.provider_request_id.empty()) {
      error.provider_request_id = context.response.provider_request_id;
    }
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
    auto error =
        transport_policy::http_error(status, context.response.provider_request_id);
    error.retry_after = curl_error::retry_after(context.response.headers);
    return std::unexpected(std::move(error));
  }
  return TransportResult{
      .status_code = status,
      .headers = std::move(context.response.headers),
      .provider_request_id = std::move(context.response.provider_request_id),
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

Status CurlTransport::status() const {
  if (impl_->startup_error()) {
    return std::unexpected(*impl_->startup_error());
  }
  return {};
}

Result<TransportResult> CurlTransport::perform(const TransportRequest& request,
                                               const std::stop_token shutdown,
                                               const std::atomic<bool>& cancelled,
                                               BodyChunkSink& body_sink) {
  if (auto status = validate_execution(impl_->startup_error(), shutdown, cancelled);
      !status) {
    return std::unexpected(std::move(status.error()));
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
      .response =
          {
              .limit = request.limits.max_response_bytes,
          },
  };
  if (auto status = configure_easy(easy.get(), request, *headers, context); !status) {
    return std::unexpected(std::move(status.error()));
  }
  MultiTransfer multi;
  if (!multi.valid() || multi.add(easy.get()) != CURLM_OK) {
    return std::unexpected(
        make_error(ErrorCategory::network, "libcurl transfer setup failed", true));
  }
  auto code = drive_transfer(multi, context, request.timeouts.shutdown);
  if (!code) {
    code.error().provider_request_id = context.response.provider_request_id;
    return std::unexpected(std::move(code.error()));
  }
  return finish_transfer(easy.get(), *code, context);
}

} // namespace scry::detail
