#pragma once

#include "core/transport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <scry/error.hpp>

namespace scry::detail::curl_error {

enum class AbortCause : std::uint8_t {
  none,
  turn_cancelled,
  harness_shutdown,
};

[[nodiscard]] Error cancelled(AbortCause cause);
[[nodiscard]] Error classify(int code, const std::optional<Error>& callback_error,
                             AbortCause abort_cause);
[[nodiscard]] std::optional<std::chrono::milliseconds>
retry_after(const std::vector<HttpHeader>& headers);

} // namespace scry::detail::curl_error
