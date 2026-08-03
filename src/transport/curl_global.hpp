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

// Initializes libcurl's process-wide state on the first call and reports
// whether the runtime is usable. Every later call returns the same answer
// without touching libcurl again.
[[nodiscard]] Status curl_global_status();

} // namespace scry::detail
