#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <scry/conversation.hpp>
#include <scry/harness.hpp>
#include <scry/json.hpp>
#include <string>
#include <utility>

namespace {

[[nodiscard]] scry::Config test_config(const std::size_t conversation_bytes = 1024) {
  scry::Config config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.limits.max_conversation_bytes = conversation_bytes;
  return config;
}

[[nodiscard]] std::unique_ptr<scry::detail::ProviderAdapter> provider() {
  auto adapter = scry::detail::make_provider_adapter(scry::ProviderDialect::anthropic);
  REQUIRE(adapter);
  return std::move(*adapter);
}

[[nodiscard]] scry::Result<scry::Harness>
fake_harness(const std::size_t conversation_bytes = 1024) {
  return scry::detail::HarnessTestAccess::create(
      test_config(conversation_bytes), provider(),
      std::make_unique<scry::test::FakeTransport>());
}

} // namespace

TEST_CASE("Conversation persistence excludes busy and uncommitted turn state") {
  auto conversation = scry::Conversation::from_json(
      {.text =
           R"({"messages":[{"content":[{"text":"committed","type":"text"}],"role":"user"}],"system_prompt":"prompt","version":1})"});
  REQUIRE(conversation);
  auto committed = conversation->to_json();
  REQUIRE(committed);

  auto first_harness = fake_harness();
  REQUIRE(first_harness);
  auto pending = first_harness->send(*conversation, "not committed");
  REQUIRE(pending);

  auto while_busy = conversation->to_json();
  REQUIRE(while_busy);
  CHECK(while_busy->text == committed->text);

  auto restored = scry::Conversation::from_json(*while_busy);
  REQUIRE(restored);
  auto second_harness = fake_harness();
  REQUIRE(second_harness);
  auto accepted = second_harness->send(*restored, "accepted because restored is idle");
  REQUIRE(accepted);

  auto bounded = scry::Conversation::from_json(*committed);
  REQUIRE(bounded);
  auto bounded_harness = fake_harness(15);
  REQUIRE(bounded_harness);
  auto over_limit = bounded_harness->send(*bounded, "x");
  REQUIRE_FALSE(over_limit);
  CHECK(over_limit.error().category == scry::ErrorCategory::resource_limit);
}
