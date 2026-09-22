#include "transport/curl_transport.hpp"

#include "core/error.hpp"
#include "transport/curl_error.hpp"
#include "transport/curl_global.hpp"
#include "transport/transport_policy.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
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

struct EasyDeleter {
  void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
};

using EasyHandle = std::unique_ptr<CURL, EasyDeleter>;

// Detaches an added easy handle at scope exit, which returns its connection to
// the multi handle's cache instead of destroying the cache with the transfer.
// It must be destroyed before the easy handle it names.
struct MultiDetach {
  CURLM* multi;
  CURL* easy;

  ~MultiDetach() { static_cast<void>(curl_multi_remove_handle(multi, easy)); }
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
  // Bounded prefix of a non-2xx body, retained only long enough to extract the
  // provider's sanitized error token. Never delivered to the sink.
  std::string error_body{};
  curl_error::AbortCause abort_cause{curl_error::AbortCause::none};
};

// Runs one libcurl data callback. A failure, returned or thrown, is recorded on
// the context and reported to libcurl as a short write, which aborts the
// transfer; no exception crosses back into C.
template <typename Accept>
[[nodiscard]] std::size_t guarded_callback(void* userdata, const std::string_view data,
                                           const char* exception_message,
                                           Accept accept) noexcept {
  auto& context = *static_cast<TransferContext*>(userdata);
  try {
    auto status = accept(context, data);
    if (!status) {
      context.callback_error = std::move(status.error());
      return 0;
    }
    return data.size();
  } catch (...) {
    context.callback_error = make_error(ErrorCategory::protocol, exception_message);
    return 0;
  }
}

[[nodiscard]] Status accept_header(TransferContext& context,
                                   const std::string_view line) {
  return context.response.accept_header(line);
}

[[nodiscard]] Status accept_body(TransferContext& context,
                                 const std::string_view chunk) {
  if (auto status = context.response.account_body(chunk.size()); !status) {
    return status;
  }
  if (!context.response.deliver_body) {
    transport_policy::append_error_body(context.error_body, chunk);
    return {};
  }
  auto status = (*context.body_sink)(chunk);
  if (!status) {
    auto error = std::move(status.error());
    error.message = "response consumer rejected response data";
    error.provider_detail =
        transport_policy::sanitize_provider_detail(error.provider_detail);
    return std::unexpected(std::move(error));
  }
  return {};
}

std::size_t header_callback(char* data, const std::size_t size, const std::size_t count,
                            void* userdata) noexcept {
  return guarded_callback(userdata, std::string_view{data, size * count},
                          "response header processing failed", accept_header);
}

std::size_t body_callback(char* data, const std::size_t size, const std::size_t count,
                          void* userdata) noexcept {
  return guarded_callback(userdata, std::string_view{data, size * count},
                          "response body processing failed", accept_body);
}

// Records why the transfer is being abandoned so the eventual
// CURLE_ABORTED_BY_CALLBACK can be reported as the right kind of cancellation.
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

int progress_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t,
                      curl_off_t) noexcept {
  return cancellation_requested(*static_cast<TransferContext*>(userdata)) ? 1 : 0;
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

// A millisecond count clamped to what libcurl's `long` or `int` parameter holds.
template <std::integral Target>
[[nodiscard]] Target
saturated_milliseconds(const std::chrono::milliseconds value) noexcept {
  using Rep = std::chrono::milliseconds::rep;
  return static_cast<Target>(std::min<Rep>(
      value.count(), static_cast<Rep>(std::numeric_limits<Target>::max())));
}

// Curl expresses the low-speed (idle) bound in whole seconds, so a sub-second
// value rounds up rather than disabling the bound.
[[nodiscard]] long
idle_timeout_seconds(const std::chrono::milliseconds value) noexcept {
  using Rep = std::chrono::seconds::rep;
  const auto seconds = std::chrono::ceil<std::chrono::seconds>(value).count();
  return std::max(1L,
                  static_cast<long>(std::min<Rep>(
                      seconds, static_cast<Rep>(std::numeric_limits<long>::max()))));
}

// Applies a flat sequence of easy-handle options, stopping at the first
// failure. Every one of these failures is effectively impossible, so they
// share one diagnostic rather than one branch each.
class EasyOptions final {
public:
  explicit EasyOptions(CURL* easy) noexcept : easy_(easy) {}

  template <typename Value>
  EasyOptions& set(const CURLoption option, Value value) noexcept {
    if (code_ == CURLE_OK) {
      code_ = curl_easy_setopt(easy_, option, value);
    }
    return *this;
  }

  [[nodiscard]] Status status() const {
    if (code_ == CURLE_OK) {
      return {};
    }
    return std::unexpected(
        make_error(ErrorCategory::protocol, "libcurl request setup failed"));
  }

private:
  CURL* easy_{};
  CURLcode code_{CURLE_OK};
};

[[nodiscard]] Status configure_easy(CURL* easy, const TransportRequest& request,
                                    HeaderList& headers, TransferContext& context) {
  EasyOptions options{easy};
  options.set(CURLOPT_URL, request.url.c_str())
      .set(CURLOPT_POST, 1L)
      .set(CURLOPT_POSTFIELDS, request.body.data())
      .set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()))
      .set(CURLOPT_HTTPHEADER, headers.get())
      .set(CURLOPT_NOSIGNAL, 1L)
      .set(CURLOPT_MAXFILESIZE_LARGE,
           static_cast<curl_off_t>(request.limits.max_response_bytes))
      .set(CURLOPT_WRITEFUNCTION, body_callback)
      .set(CURLOPT_WRITEDATA, &context)
      .set(CURLOPT_HEADERFUNCTION, header_callback)
      .set(CURLOPT_HEADERDATA, &context)
      .set(CURLOPT_XFERINFOFUNCTION, progress_callback)
      .set(CURLOPT_XFERINFODATA, &context)
      .set(CURLOPT_NOPROGRESS, 0L)
      .set(CURLOPT_CONNECTTIMEOUT_MS,
           saturated_milliseconds<long>(request.timeouts.connect))
      .set(CURLOPT_LOW_SPEED_LIMIT, 1L)
      .set(CURLOPT_LOW_SPEED_TIME, idle_timeout_seconds(request.timeouts.idle))
      .set(CURLOPT_SSL_VERIFYPEER, request.tls_verify_peer ? 1L : 0L)
      .set(CURLOPT_SSL_VERIFYHOST, request.tls_verify_peer ? 2L : 0L);
  // Leaving CURLOPT_TIMEOUT_MS unset keeps curl's default of no total bound,
  // which is what an unset transfer timeout means.
  if (request.timeouts.transfer) {
    options.set(CURLOPT_TIMEOUT_MS,
                saturated_milliseconds<long>(*request.timeouts.transfer));
  }
  // Same pattern for the optional network options: an empty value leaves
  // libcurl's default trust store and its proxy environment handling alone.
  if (!request.ca_bundle_path.empty()) {
    options.set(CURLOPT_CAINFO, request.ca_bundle_path.c_str());
  }
  if (!request.proxy.empty()) {
    options.set(CURLOPT_PROXY, request.proxy.c_str());
  }
  return options.status();
}

[[nodiscard]] Status validate_execution(Status startup_status,
                                        const std::stop_token& shutdown,
                                        const std::atomic<bool>& cancelled) {
  if (!startup_status) {
    return std::unexpected(std::move(startup_status.error()));
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

[[nodiscard]] Result<CURLcode>
drive_transfer(CURLM* multi, TransferContext& context,
               const std::chrono::milliseconds shutdown_bound) {
  int running = 0;
  while (true) {
    if (cancellation_requested(context)) {
      return CURLE_ABORTED_BY_CALLBACK;
    }
    if (curl_multi_perform(multi, &running) != CURLM_OK) {
      return std::unexpected(
          make_error(ErrorCategory::network, "libcurl transfer driver failed", true));
    }
    if (running == 0) {
      break;
    }
    int ready = 0;
    if (curl_multi_poll(multi, nullptr, 0, saturated_milliseconds<int>(shutdown_bound),
                        &ready) != CURLM_OK) {
      return std::unexpected(
          make_error(ErrorCategory::network, "libcurl transfer wait failed", true));
    }
  }

  int messages_remaining = 0;
  while (auto* message = curl_multi_info_read(multi, &messages_remaining)) {
    if (message->msg == CURLMSG_DONE) {
      return message->data.result;
    }
  }
  return std::unexpected(
      make_error(ErrorCategory::protocol, "libcurl transfer result is missing"));
}

[[nodiscard]] Result<TransportResult> finish_transfer(CURL* easy, const CURLcode code,
                                                      TransferContext& context,
                                                      const TransportRequest& request) {
  if (code != CURLE_OK) {
    auto error = curl_error::classify(static_cast<int>(code), context.callback_error,
                                      context.abort_cause);
    if (error.provider_request_id.empty()) {
      error.provider_request_id = context.response.provider_request_id;
    }
    // An in-stream provider error arrives on a 2xx response, so carry the
    // status it arrived on rather than reporting no HTTP response at all.
    if (error.http_status == 0 && context.response.status_code != 0) {
      error.http_status = static_cast<std::uint16_t>(context.response.status_code);
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
        transport_policy::http_error(status, context.response.provider_request_id,
                                     context.error_body, request.provider_namespace);
    error.retry_after = curl_error::retry_after(context.response.retry_after_values);
    return std::unexpected(std::move(error));
  }
  return TransportResult{
      .status_code = status,
      .provider_request_id = std::move(context.response.provider_request_id),
  };
}

} // namespace

// Constructing a transport is what first initializes libcurl's process-wide
// state, so a failure is observable through status() before any transfer.
CurlTransport::CurlTransport() { static_cast<void>(curl_global_status()); }

CurlTransport::~CurlTransport() {
  if (multi_ != nullptr) {
    static_cast<void>(curl_multi_cleanup(multi_));
  }
}

Status CurlTransport::status() const { return curl_global_status(); }

// Creates the multi handle once and keeps it for the transport's lifetime,
// because it holds libcurl's connection cache: reusing it is what lets a retry
// or a later turn skip the TCP and TLS handshake. Null on failure.
CURLM* CurlTransport::multi() {
  if (multi_ == nullptr) {
    multi_ = curl_multi_init();
  }
  return multi_;
}

Result<TransportResult> CurlTransport::perform(const TransportRequest& request,
                                               const std::stop_token shutdown,
                                               const std::atomic<bool>& cancelled,
                                               BodyChunkSink& body_sink) {
  if (auto status = validate_execution(curl_global_status(), shutdown, cancelled);
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
  auto* multi = this->multi();
  if (multi == nullptr || curl_multi_add_handle(multi, easy.get()) != CURLM_OK) {
    return std::unexpected(
        make_error(ErrorCategory::network, "libcurl transfer setup failed", true));
  }
  const MultiDetach detach{.multi = multi, .easy = easy.get()};
  auto code = drive_transfer(multi, context, request.timeouts.shutdown);
  if (!code) {
    code.error().provider_request_id = context.response.provider_request_id;
    return std::unexpected(std::move(code.error()));
  }
  return finish_transfer(easy.get(), *code, context, request);
}

} // namespace scry::detail
