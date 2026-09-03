#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <scry/config.hpp>
#include <scry/error.hpp>
#include <scry/turn_id.hpp>

namespace scry::detail {

[[nodiscard]] bool is_retryable(ErrorCategory category) noexcept;

[[nodiscard]] double retry_jitter_sample(std::uint64_t seed, TurnId turn_id,
                                         std::uint32_t failed_attempt) noexcept;

[[nodiscard]] std::chrono::milliseconds
retry_delay(const RetryPolicy& policy, std::uint32_t failed_attempt,
            std::optional<std::chrono::milliseconds> retry_after,
            double jitter_sample) noexcept;

} // namespace scry::detail
