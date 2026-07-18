#include "core/retry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <scry/error.hpp>

using namespace std::chrono_literals;

TEST_CASE("retry classifier accepts only transient categories") {
  using scry::ErrorCategory;
  using scry::detail::is_retryable;

  CHECK(is_retryable(ErrorCategory::rate_limit));
  CHECK(is_retryable(ErrorCategory::network));

  CHECK_FALSE(is_retryable(ErrorCategory::invalid_config));
  CHECK_FALSE(is_retryable(ErrorCategory::invalid_state));
  CHECK_FALSE(is_retryable(ErrorCategory::busy));
  CHECK_FALSE(is_retryable(ErrorCategory::authentication));
  CHECK_FALSE(is_retryable(ErrorCategory::protocol));
  CHECK_FALSE(is_retryable(ErrorCategory::resource_limit));
  CHECK_FALSE(is_retryable(ErrorCategory::tool));
  CHECK_FALSE(is_retryable(ErrorCategory::max_tool_rounds));
  CHECK_FALSE(is_retryable(ErrorCategory::cancelled));
}

TEST_CASE("retry delay is exponential, jittered, and capped") {
  const scry::RetryPolicy policy{
      .initial_backoff = 250ms,
      .max_backoff = 2s,
      .jitter_ratio = 0.2,
  };

  CHECK(scry::detail::retry_delay(policy, 1, std::nullopt, 0.0) == 250ms);
  CHECK(scry::detail::retry_delay(policy, 2, std::nullopt, 0.0) == 500ms);
  CHECK(scry::detail::retry_delay(policy, 3, std::nullopt, 1.0) == 1200ms);
  CHECK(scry::detail::retry_delay(policy, 4, std::nullopt, 1.0) == 2s);
}

TEST_CASE("retry-after is honored without exceeding the configured cap") {
  const scry::RetryPolicy policy{
      .initial_backoff = 100ms,
      .max_backoff = 2s,
      .jitter_ratio = 0.0,
  };

  CHECK(scry::detail::retry_delay(policy, 1, 1500ms, 0.0) == 1500ms);
  CHECK(scry::detail::retry_delay(policy, 1, 10s, 0.0) == 2s);
}
