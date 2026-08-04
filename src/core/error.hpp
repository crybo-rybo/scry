#pragma once

#include <scry/error.hpp>
#include <string>

namespace scry::detail {

// The library's single private Error factory. Keeping it in the bottom layer
// lets core, provider, and transport code construct errors without depending
// on an unrelated subsystem such as JSON.
[[nodiscard]] Error make_error(ErrorCategory category, std::string message,
                               bool retryable = false);

} // namespace scry::detail
