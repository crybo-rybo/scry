#include "transport/curl_global.hpp"

#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace scry::detail {
namespace {

constexpr auto minimum_curl_version = CURL_VERSION_BITS(7, 84, 0);

[[nodiscard]] Error startup_error(const ErrorCategory category, std::string message) {
  return Error{
      .category = category,
      .message = std::move(message),
  };
}

} // namespace

class CurlGlobalState final {
public:
  CurlGlobalState() {
    const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
      error_ =
          startup_error(ErrorCategory::network, "libcurl global initialization failed");
      return;
    }
    initialized_ = true;
    validate_runtime();
  }

  ~CurlGlobalState() {
    if (initialized_) {
      curl_global_cleanup();
    }
  }

  CurlGlobalState(const CurlGlobalState&) = delete;
  CurlGlobalState& operator=(const CurlGlobalState&) = delete;

  [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

private:
  void validate_runtime() {
    const auto* version = curl_version_info(CURLVERSION_NOW);
    if (version == nullptr || version->version_num < minimum_curl_version) {
      error_ = startup_error(ErrorCategory::invalid_config,
                             "libcurl 7.84 or newer is required");
      return;
    }
    if ((version->features & CURL_VERSION_THREADSAFE) == 0) {
      error_ = startup_error(ErrorCategory::invalid_config,
                             "libcurl must support thread-safe global initialization");
      return;
    }
    if ((version->features & CURL_VERSION_ASYNCHDNS) == 0) {
      error_ = startup_error(ErrorCategory::invalid_config,
                             "libcurl must provide asynchronous DNS resolution");
    }
  }

  bool initialized_{false};
  std::optional<Error> error_{};
};

namespace {

class CurlGlobalRegistry final {
public:
  [[nodiscard]] std::shared_ptr<CurlGlobalState> acquire() {
    const std::scoped_lock lock{mutex_};
    auto lease = state_.lock();
    if (!lease) {
      lease = std::make_shared<CurlGlobalState>();
      state_ = lease;
    }
    return lease;
  }

private:
  std::mutex mutex_{};
  std::weak_ptr<CurlGlobalState> state_{};
};

[[nodiscard]] std::shared_ptr<CurlGlobalState> acquire_curl_global() {
  static CurlGlobalRegistry registry;
  return registry.acquire();
}

} // namespace

CurlGlobalLease::CurlGlobalLease() : state_(acquire_curl_global()) {}

CurlGlobalLease::~CurlGlobalLease() = default;

CurlGlobalLease::CurlGlobalLease(CurlGlobalLease&&) noexcept = default;

CurlGlobalLease& CurlGlobalLease::operator=(CurlGlobalLease&&) noexcept = default;

const std::optional<Error>& CurlGlobalLease::error() const noexcept {
  return state_->error();
}

} // namespace scry::detail
