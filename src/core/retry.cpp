#include "core/retry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace scry::detail {
namespace {

using MillisecondsRep = std::chrono::milliseconds::rep;

[[nodiscard]] std::chrono::milliseconds
exponential_delay(const RetryPolicy& policy, std::uint32_t failed_attempt) noexcept {
  constexpr auto rep_max = std::numeric_limits<MillisecondsRep>::max();
  const auto initial = std::max(policy.initial_backoff.count(), MillisecondsRep{0});
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

[[nodiscard]] MillisecondsRep saturating_round(const std::chrono::milliseconds base,
                                               const double factor) noexcept {
  constexpr auto rep_max = std::numeric_limits<MillisecondsRep>::max();
  const auto scaled = static_cast<double>(base.count()) * factor;
  const auto upper_bound = static_cast<double>(rep_max);
  if (!std::isfinite(scaled) || scaled >= upper_bound) {
    return rep_max;
  }
  return static_cast<MillisecondsRep>(std::llround(scaled));
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
  const auto jittered_count = saturating_round(base, factor);
  auto delay = std::chrono::milliseconds{
      std::max(jittered_count, std::chrono::milliseconds::rep{0})};
  if (retry_after) {
    delay = std::max(delay, std::max(*retry_after, std::chrono::milliseconds{0}));
  }
  return std::min(delay, std::max(policy.max_backoff, std::chrono::milliseconds{0}));
}

} // namespace scry::detail
