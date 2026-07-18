#include "core/retry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace scry::detail {
namespace {

[[nodiscard]] std::chrono::milliseconds
exponential_delay(const RetryPolicy& policy, std::uint32_t failed_attempt) noexcept {
  using Rep = std::chrono::milliseconds::rep;
  constexpr auto rep_max = std::numeric_limits<Rep>::max();
  const auto initial = std::max(policy.initial_backoff.count(), Rep{0});
  auto value = initial;
  for (std::uint32_t attempt = 1; attempt < failed_attempt; ++attempt) {
    if (value > rep_max / 2) {
      value = rep_max;
      break;
    }
    value *= 2;
  }
  return std::min(std::chrono::milliseconds{value},
                  std::max(policy.max_backoff, std::chrono::milliseconds{0}));
}

} // namespace

bool is_retryable(const ErrorCategory category) noexcept {
  return category == ErrorCategory::rate_limit || category == ErrorCategory::network;
}

std::chrono::milliseconds
retry_delay(const RetryPolicy& policy, const std::uint32_t failed_attempt,
            const std::optional<std::chrono::milliseconds> retry_after,
            const double jitter_sample) noexcept {
  const auto base =
      exponential_delay(policy, std::max(failed_attempt, std::uint32_t{1}));
  const auto bounded_sample = std::clamp(jitter_sample, -1.0, 1.0);
  const auto bounded_ratio = std::clamp(policy.jitter_ratio, 0.0, 1.0);
  const auto factor = 1.0 + (bounded_ratio * bounded_sample);
  const auto jittered_count = static_cast<std::chrono::milliseconds::rep>(
      std::llround(static_cast<double>(base.count()) * factor));
  auto delay = std::chrono::milliseconds{
      std::max(jittered_count, std::chrono::milliseconds::rep{0})};
  if (retry_after) {
    delay = std::max(delay, std::max(*retry_after, std::chrono::milliseconds{0}));
  }
  return std::min(delay, std::max(policy.max_backoff, std::chrono::milliseconds{0}));
}

} // namespace scry::detail
