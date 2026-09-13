#pragma once

#include <cstdint>
#include <scry/error.hpp>

namespace scry::detail {

struct CurlRuntimeCapabilities {
  std::uint32_t version_number{};
  bool thread_safe{};
  bool asynchronous_dns{};
};

[[nodiscard]] Status
validate_curl_runtime_capabilities(CurlRuntimeCapabilities capabilities);

// Initializes libcurl's process-wide state on the first call and reports the
// result. A function-static owner serializes that one attempt and caches its
// result, including a failure; its destructor pairs a successful startup with
// exactly one cleanup at static teardown. Capability failure after a successful
// initialization is cleaned up immediately.
[[nodiscard]] Status curl_global_status();

} // namespace scry::detail
