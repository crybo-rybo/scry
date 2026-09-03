#pragma once

#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

// The canonical five-event Anthropic text completion, built once instead of
// retyped in every suite that needs a plain successful turn. Streams carrying
// tool blocks, oversized deltas, or errors stay next to the tests that assert on
// their specific shape.
[[nodiscard]] inline std::string anthropic_text_stream(
    const std::string_view text, const std::string_view message_id = "msg_test",
    const std::string_view request_id = {}, const std::uint32_t input_tokens = 2,
    const std::uint32_t output_tokens = 2) {
  auto correlation = std::string{};
  if (!request_id.empty()) {
    correlation = R"(,"request_id":")" + std::string{request_id} + R"(")";
  }
  auto stream = std::string{"event: message_start\ndata: "};
  stream += R"({"type":"message_start","message":{"id":")";
  stream += message_id;
  stream += R"(")";
  stream += correlation;
  stream += R"(,"type":"message","role":"assistant","content":[],)";
  stream += R"("model":"test-model","stop_reason":null,"usage":{"input_tokens":)";
  stream += std::to_string(input_tokens);
  stream += R"(,"output_tokens":0}}})";
  stream += "\n\nevent: content_block_start\ndata: ";
  stream += R"({"type":"content_block_start","index":0,)";
  stream += R"("content_block":{"type":"text","text":""}})";
  stream += "\n\nevent: content_block_delta\ndata: ";
  stream += R"({"type":"content_block_delta","index":0,)";
  stream += R"("delta":{"type":"text_delta","text":")";
  stream += text;
  stream += R"("}})";
  stream += "\n\nevent: content_block_stop\ndata: ";
  stream += R"({"type":"content_block_stop","index":0})";
  stream += "\n\nevent: message_delta\ndata: ";
  stream += R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},)";
  stream += R"("usage":{"output_tokens":)";
  stream += std::to_string(output_tokens);
  stream += R"(}})";
  stream += "\n\nevent: message_stop\ndata: ";
  stream += R"({"type":"message_stop"})";
  stream += "\n\n";
  return stream;
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

// A Harness over a scripted FakeTransport plus the Conversation to drive it.
// The transport pointer stays valid for the fixture's lifetime because the
// Harness owns the transport it was created with.
struct HarnessFixture {
  scry::test::FakeTransport* transport;
  scry::Harness harness;
  scry::Conversation conversation;
};

[[nodiscard]] inline HarnessFixture make_harness_fixture(
    scry::Config config, std::vector<scry::test::ScriptedExchange> exchanges,
    const scry::ProviderDialect dialect = scry::ProviderDialect::anthropic) {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* observer = fake.get();
  for (auto& exchange : exchanges) {
    fake->enqueue(std::move(exchange));
  }
  auto harness = scry::detail::HarnessTestAccess::create(
      std::move(config), provider(dialect), std::move(fake));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  return HarnessFixture{
      .transport = observer,
      .harness = std::move(*harness),
      .conversation = std::move(*conversation),
  };
}

} // namespace scry::test_support
