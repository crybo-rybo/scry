#include "transport/curl_global.hpp"

#include "core/json_codec.hpp"

#include <curl/curl.h>

namespace scry::detail {
namespace {

constexpr auto minimum_curl_version = CURL_VERSION_BITS(7, 84, 0);

[[nodiscard]] Status initialize_curl_global() {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    return std::unexpected(
        make_error(ErrorCategory::network, "libcurl global initialization failed"));
  }
  const auto* version = curl_version_info(CURLVERSION_NOW);
  return validate_curl_runtime_capabilities({
      .version_number = version == nullptr ? 0U : version->version_num,
      .thread_safe =
          version != nullptr && (version->features & CURL_VERSION_THREADSAFE) != 0,
      .asynchronous_dns =
          version != nullptr && (version->features & CURL_VERSION_ASYNCHDNS) != 0,
  });
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
  // The required libcurl (7.84 with CURL_VERSION_THREADSAFE) initializes its
  // global state safely from any thread, so one process-wide initialization is
  // enough. There is deliberately no matching curl_global_cleanup: releasing
  // the last transport must not tear down TLS state a host still needs when it
  // cycles harnesses.
  static const Status status = initialize_curl_global();
  return status;
}

} // namespace scry::detail
