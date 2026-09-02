/// @file
/// @brief Pure retry classification and delay calculation utilities.
///
/// The turn machine owns retry state; these helpers contain only deterministic
/// policy and perform no sleeping or clock access.

#pragma once

#include <chrono>
#include <optional>
#include <scry/config.hpp>
#include <scry/error.hpp>

namespace scry::detail {

/// @brief Reports whether an error category is eligible for automatic retry.
/// @param category Category produced by a lower layer.
/// @return `true` only for rate-limit and network failures.
[[nodiscard]] bool is_retryable(ErrorCategory category) noexcept;

/// @brief Computes the bounded exponential delay after a failed attempt.
///
/// The calculation doubles the initial backoff per attempt, applies a clamped
/// jitter sample and ratio, honors a nonnegative `Retry-After` lower bound, and
/// finally caps the result at the nonnegative configured maximum. Arithmetic
/// saturates rather than overflowing.
///
/// @param policy Retry limits and backoff parameters.
/// @param failed_attempt One-based number of the failed attempt; zero is
///        treated as one defensively.
/// @param retry_after Optional provider-requested minimum delay.
/// @param jitter_sample Deterministic sample in `[-1, 1]`; out-of-range values
///        are clamped.
/// @return A nonnegative delay no greater than `policy.max_backoff`.
[[nodiscard]] std::chrono::milliseconds
retry_delay(const RetryPolicy& policy, std::uint32_t failed_attempt,
            std::optional<std::chrono::milliseconds> retry_after,
            double jitter_sample) noexcept;

} // namespace scry::detail
