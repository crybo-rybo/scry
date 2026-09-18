#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <scry/error.hpp>
#include <string>
#include <vector>

namespace scry::detail::curl_error {

enum class AbortCause : std::uint8_t {
  none,
  turn_cancelled,
  harness_shutdown,
};

[[nodiscard]] Error cancelled(AbortCause cause);
[[nodiscard]] Error classify(int code, const std::optional<Error>& callback_error,
                             AbortCause abort_cause);
// Resolves the response's Retry-After delay: the first of the received values
// that parses as a delta-seconds count or an HTTP date, and nothing when none
// does.
[[nodiscard]] std::optional<std::chrono::milliseconds>
retry_after(const std::vector<std::string>& values);

} // namespace scry::detail::curl_error
