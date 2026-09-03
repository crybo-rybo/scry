#include "runtime/config.hpp"
#include "runtime/startup.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <scry/config.hpp>
#include <scry/error.hpp>
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
  config.base_url = "https://example .test";
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

TEST_CASE("OpenAI-compatible configuration accepts local servers without auth") {
  auto config = valid_config();
  config.dialect = scry::ProviderDialect::openai_compatible;
  config.api_key.clear();
  config.sampling.temperature = 2.0;
  config.sampling.top_p = 0.0;
  CHECK(scry::detail::validate_config(config));

  config.api_key = "unsafe\r\nheader";
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("configuration rejects unknown provider dialects") {
  auto config = valid_config();
  config.dialect =
      static_cast<scry::ProviderDialect>(std::numeric_limits<std::uint8_t>::max());
  const auto result = scry::detail::validate_config(config);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == scry::ErrorCategory::invalid_config);
}

TEST_CASE("reasoning disablement is restricted to OpenAI-compatible requests") {
  auto config = valid_config();
  config.reasoning_mode = scry::ReasoningMode::disabled;
  const auto unsupported = scry::detail::validate_config(config);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error().message ==
        "reasoning_mode = disabled requires the OpenAI-compatible provider dialect");

  config.dialect = scry::ProviderDialect::openai_compatible;
  CHECK(scry::detail::validate_config(config));

  config.reasoning_mode =
      static_cast<scry::ReasoningMode>(std::numeric_limits<std::uint8_t>::max());
  CHECK_FALSE(scry::detail::validate_config(config));
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
  const auto unset_max_tokens = scry::detail::validate_config(config);
  REQUIRE_FALSE(unset_max_tokens);
  CHECK(unset_max_tokens.error().message ==
        "Anthropic max_tokens must be set and greater than 0; the Messages API "
        "requires it");

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

TEST_CASE("configuration applies provider-specific sampling bounds") {
  auto config = valid_config();
  config.sampling.temperature = 1.5;
  CHECK_FALSE(scry::detail::validate_config(config));

  config.dialect = scry::ProviderDialect::openai_compatible;
  CHECK(scry::detail::validate_config(config));

  config.sampling.temperature = 2.01;
  CHECK_FALSE(scry::detail::validate_config(config));

  config.sampling.temperature = 1.0;
  config.sampling.top_p = 0.0;
  CHECK(scry::detail::validate_config(config));

  config.sampling.top_p = -0.01;
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("Anthropic sampling rejects every invalid numeric shape") {
  auto config = valid_config();

  for (const auto temperature : {std::numeric_limits<double>::quiet_NaN(), -0.1, 1.1}) {
    config.sampling.temperature = temperature;
    CHECK_FALSE(scry::detail::validate_config(config));
  }

  config.sampling.temperature = 0.5;
  for (const auto top_p : {std::numeric_limits<double>::quiet_NaN(), 0.0, -0.1, 1.1}) {
    config.sampling.top_p = top_p;
    CHECK_FALSE(scry::detail::validate_config(config));
  }
}

TEST_CASE("OpenAI-compatible sampling rejects every invalid numeric shape") {
  auto config = valid_config();
  config.dialect = scry::ProviderDialect::openai_compatible;

  for (const auto temperature :
       {std::numeric_limits<double>::quiet_NaN(), -0.01, 2.01}) {
    config.sampling.temperature = temperature;
    CHECK_FALSE(scry::detail::validate_config(config));
  }

  config.sampling.temperature = 1.0;
  for (const auto top_p : {std::numeric_limits<double>::quiet_NaN(), -0.01, 1.01}) {
    config.sampling.top_p = top_p;
    CHECK_FALSE(scry::detail::validate_config(config));
  }

  config.sampling.top_p = 0.5;
  // An unset max_tokens is valid for this dialect: the field is omitted and the
  // server default applies. Zero is still rejected.
  config.sampling.max_tokens.reset();
  CHECK(scry::detail::validate_config(config));
  config.sampling.max_tokens = 0;
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("configuration rejects zero timeouts, undersized limits, and zero tool "
          "rounds") {
  using namespace std::chrono_literals;

  auto config = valid_config();
  config.timeouts.shutdown = {};
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.timeouts.connect = {};
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.timeouts.idle = {};
  CHECK_FALSE(scry::detail::validate_config(config));

  // An unset total transfer bound is the default and stays accepted; a set one
  // must still be positive.
  config = valid_config();
  config.timeouts.transfer = {};
  CHECK(scry::detail::validate_config(config));

  config = valid_config();
  config.timeouts.transfer = 0ms;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.timeouts.transfer = -1ms;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.limits.max_queued_event_bytes_per_turn = 1023;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.max_tool_rounds = 0;
  CHECK_FALSE(scry::detail::validate_config(config));
}

TEST_CASE("configuration rejects a zero value for every resource limit") {
  constexpr std::array limits{
      &scry::ResourceLimits::max_pending_turns,
      &scry::ResourceLimits::max_sse_event_bytes,
      &scry::ResourceLimits::max_response_bytes,
      &scry::ResourceLimits::max_tool_arguments_bytes,
      &scry::ResourceLimits::max_tool_result_bytes,
      &scry::ResourceLimits::max_queued_event_bytes_per_turn,
      &scry::ResourceLimits::max_conversation_bytes,
  };
  for (const auto member : limits) {
    auto config = valid_config();
    config.limits.*member = 0;
    const auto status = scry::detail::validate_config(config);
    REQUIRE_FALSE(status);
    CHECK(status.error().category == scry::ErrorCategory::invalid_config);
  }
}

TEST_CASE("configuration rejects negative retry backoffs and out-of-range jitter") {
  using namespace std::chrono_literals;

  auto config = valid_config();
  config.retry.initial_backoff = -1ms;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.retry.max_backoff = -1ms;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.retry.max_elapsed = -1ms;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.retry.jitter_ratio = std::numeric_limits<double>::infinity();
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.retry.jitter_ratio = -0.1;
  CHECK_FALSE(scry::detail::validate_config(config));

  config = valid_config();
  config.retry.jitter_ratio = 1.1;
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
