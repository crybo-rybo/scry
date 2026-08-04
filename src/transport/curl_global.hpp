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

// Owns libcurl's process-wide state from the first call until static teardown.
// The first result (including failure) is cached; capability failure after a
// successful initialization is cleaned up immediately. Successful startup is
// paired with exactly one cleanup from the function-static owner's destructor.
[[nodiscard]] Status curl_global_status();

} // namespace scry::detail
