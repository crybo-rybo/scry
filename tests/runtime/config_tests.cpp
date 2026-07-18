#include "runtime/config.hpp"
#include "runtime/startup.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <new>
#include <scry/config.hpp>
#include <string>
#include <system_error>

namespace {

[[nodiscard]] scry::Config valid_config() {
  return {
      .base_url = "https://example.test",
      .api_key = "test-key",
      .model = "model",
  };
}

} // namespace

TEST_CASE("valid Anthropic configuration is accepted") {
  CHECK(scry::detail::validate_config(valid_config()));
}

TEST_CASE("configuration rejects missing endpoint and model") {
  auto config = valid_config();
  config.base_url.clear();
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.base_url = "https:///";
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.base_url = "https://example.test?version=1";
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.base_url = "https://example.test#fragment";
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.api_key.clear();
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.api_key = "unsafe\r\nheader";
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.model.clear();
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("configuration rejects the M4 provider dialect") {
  auto config = valid_config();
  config.dialect = scry::ProviderDialect::openai_compatible;
  const auto result = scry::detail::validate_config(config);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::invalid_config);
}

TEST_CASE("configuration validates sampling and retries") {
  auto config = valid_config();
  config.sampling.temperature = std::numeric_limits<double>::infinity();
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.sampling.temperature = 1.01;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.sampling.top_p = 0.0;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.sampling.max_tokens.reset();
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.sampling.max_tokens = 0;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.retry.max_attempts = 0;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.retry.initial_backoff =
      config.retry.max_backoff + std::chrono::milliseconds{1};
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("configuration rejects zero timeouts and limits") {
  auto config = valid_config();
  config.timeouts.shutdown = {};
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.limits.max_queued_event_bytes_per_turn = 0;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.limits.max_queued_event_bytes_per_turn = 1023;
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE(
    "worker thread startup failures are translated without hiding allocation failure") {
  const auto unavailable = scry::detail::translate_worker_start_failure<int>([] {
    throw std::system_error{
        std::make_error_code(std::errc::resource_unavailable_try_again)};
    return 0;
  });
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().category == scry::ErrorCategory::resource_limit);
  CHECK(unavailable.error().message == "Harness worker thread could not be started");

  CHECK_THROWS_AS(scry::detail::translate_worker_start_failure<int>([] {
                    throw std::bad_alloc{};
                    return 0;
                  }),
                  std::bad_alloc);
}
