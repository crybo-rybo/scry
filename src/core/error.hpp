#pragma once

#include <scry/error.hpp>
#include <string>

namespace scry::detail {

// The library's shared private Error factory. It lives in the bottom layer so
// every layer can construct errors without depending on an unrelated subsystem
// such as JSON.
[[nodiscard]] Error make_error(ErrorCategory category, std::string message,
                               bool retryable = false);

} // namespace scry::detail
