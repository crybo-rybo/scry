/// @file
/// @brief Implements one-shot process-wide libcurl ownership.
///
/// A function-static RAII owner serializes the first initialization attempt,
/// validates version/thread-safety/asynchronous-DNS capabilities, caches either
/// outcome, and pairs successful startup with exactly one static cleanup.

#include "transport/curl_global.hpp"

#include "core/error.hpp"

#include <curl/curl.h>

namespace scry::detail {
namespace {

constexpr auto minimum_curl_version = CURL_VERSION_BITS(7, 84, 0);

class CurlGlobalOwner final {
public:
  CurlGlobalOwner() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      status_ = std::unexpected(
          make_error(ErrorCategory::network, "libcurl global initialization failed"));
      return;
    }
    initialized_ = true;

    const auto* version = curl_version_info(CURLVERSION_NOW);
    status_ = validate_curl_runtime_capabilities({
        .version_number = version == nullptr ? 0U : version->version_num,
        .thread_safe =
            version != nullptr && (version->features & CURL_VERSION_THREADSAFE) != 0,
        .asynchronous_dns =
            version != nullptr && (version->features & CURL_VERSION_ASYNCHDNS) != 0,
    });
    if (!status_) {
      curl_global_cleanup();
      initialized_ = false;
    }
  }

  CurlGlobalOwner(const CurlGlobalOwner&) = delete;
  CurlGlobalOwner& operator=(const CurlGlobalOwner&) = delete;

  ~CurlGlobalOwner() {
    if (initialized_) {
      curl_global_cleanup();
    }
  }

  [[nodiscard]] Status status() const { return status_; }

private:
  Status status_{};
  bool initialized_{false};
};

[[nodiscard]] CurlGlobalOwner& curl_global_owner() {
  static CurlGlobalOwner owner{};
  return owner;
}

} // namespace

Status validate_curl_runtime_capabilities(const CurlRuntimeCapabilities capabilities) {
  if (capabilities.version_number < minimum_curl_version) {
    return std::unexpected(
        make_error(ErrorCategory::invalid_config, "libcurl 7.84 or newer is required"));
  }
  if (!capabilities.thread_safe) {
    return std::unexpected(
        make_error(ErrorCategory::invalid_config,
                   "libcurl must support thread-safe global initialization"));
  }
  if (!capabilities.asynchronous_dns) {
    return std::unexpected(
        make_error(ErrorCategory::invalid_config,
                   "libcurl must provide asynchronous DNS resolution"));
  }
  return {};
}

Status curl_global_status() {
  // Function-static initialization serializes the one process-wide attempt.
  // Its result, including the first failure, is stable for the rest of the
  // process. The owner's destructor pairs a successful initialization with
  // exactly one cleanup after ordinary library objects have been destroyed.
  return curl_global_owner().status();
}

} // namespace scry::detail
