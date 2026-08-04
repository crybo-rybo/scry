#pragma once

#include "core/provider.hpp"
#include "support/transport/fake_transport.hpp"

#include <cstddef>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Helpers shared by every suite that drives a real Harness over a scripted
// transport. Suites that need a deliberately different shape - a live curl
// endpoint, retries left enabled, a byte-at-a-time stream - keep their own
// variant next to the tests that depend on it.
namespace scry::test_support {

// Port 1 is never listening, so a suite that accidentally drops its scripted
// transport fails fast instead of reaching the network.
[[nodiscard]] inline scry::Config test_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] inline std::unique_ptr<scry::detail::ProviderAdapter>
provider(const scry::ProviderDialect dialect = scry::ProviderDialect::anthropic) {
  return scry::detail::make_provider_adapter(dialect);
}

[[nodiscard]] inline scry::test::ScriptedExchange
scripted_exchange(const std::string_view stream,
                  std::string request_id = "transport-request") {
  return {
      .body_chunks = {std::string{stream}},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = std::move(request_id),
          },
  };
}

[[nodiscard]] inline scry::ToolHandler static_handler(std::string result) {
  return [result = std::move(result)](scry::Json) -> scry::Result<scry::Json> {
    return scry::Json{.text = result};
  };
}

template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update());
    if (std::forward<Predicate>(predicate)()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

// Delivers at most one callback per update so a test can observe the ordering
// the pump imposes between successive deliveries.
template <typename Predicate>
[[nodiscard]] bool pump_one_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update({.max_callbacks = 1}));
    if (std::forward<Predicate>(predicate)()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

} // namespace scry::test_support
