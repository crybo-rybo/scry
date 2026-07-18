#pragma once

#include <chrono>
#include <optional>
#include <scry/config.hpp>
#include <scry/error.hpp>

namespace scry::detail {

[[nodiscard]] bool is_retryable(ErrorCategory category) noexcept;

[[nodiscard]] std::chrono::milliseconds
retry_delay(const RetryPolicy& policy, std::uint32_t failed_attempt,
            std::optional<std::chrono::milliseconds> retry_after,
            double jitter_sample) noexcept;

} // namespace scry::detail
