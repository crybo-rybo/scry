#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"
#include "world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

constexpr std::string_view tool_stream =
    R"(data: {"id":"chatcmpl-npc-tool","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant","tool_calls":[{"index":0,"id":"call-look","type":"function","function":{"name":"look","arguments":"{}"}}]},"finish_reason":null}]}

data: {"id":"chatcmpl-npc-tool","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}

data: [DONE]

)";

constexpr std::string_view final_stream =
    R"(data: {"id":"chatcmpl-npc-final","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant","content":"done"},"finish_reason":null}]}

data: {"id":"chatcmpl-npc-final","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: [DONE]

)";

[[nodiscard]] scry::Config test_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1/v1",
      .model = "showcase-test-model",
      .dialect = scry::ProviderDialect::openai_compatible,
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] std::unique_ptr<scry::detail::ProviderAdapter> provider() {
  auto created =
      scry::detail::make_provider_adapter(scry::ProviderDialect::openai_compatible);
  REQUIRE(created);
  return std::move(*created);
}

[[nodiscard]] scry::test::ScriptedExchange
scripted_exchange(const std::string_view stream, std::string request_id) {
  return {
      .body_chunks = {std::string{stream}},
      .result =
          scry::detail::TransportResult{
              .status_code = 200,
              .provider_request_id = std::move(request_id),
          },
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

} // namespace

TEST_CASE("NPC registrations execute on the update thread and resend observations") {
  auto transport = std::make_unique<scry::test::FakeTransport>();
  auto* requests = transport.get();
  transport->enqueue(scripted_exchange(tool_stream, "npc-tool-request"));
  transport->enqueue(scripted_exchange(final_stream, "npc-final-request"));
  auto created = scry::detail::HarnessTestAccess::create(test_config(), provider(),
                                                         std::move(transport));
  REQUIRE(created);
  auto harness = std::move(*created);

  auto world = std::make_shared<scry_showcase::npc::World>();
  std::optional<scry_showcase::npc::NpcTool> observed_tool;
  std::thread::id handler_thread;
  REQUIRE(scry_showcase::npc::register_world_tools(
      harness.tools(), world, [&](const scry_showcase::npc::NpcTool tool) {
        observed_tool = tool;
        handler_thread = std::this_thread::get_id();
      }));

  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Look before moving.");
  REQUIRE(turn);

  std::optional<scry::Completion> completion;
  std::optional<scry::Error> error;
  REQUIRE(
      turn->on_complete([&](const scry::Completion& value) { completion = value; }));
  REQUIRE(turn->on_error([&](const scry::Error& value) { error = value; }));
  REQUIRE(
      pump_until(harness, [&] { return completion.has_value() || error.has_value(); }));

  REQUIRE_FALSE(error);
  REQUIRE(completion);
  REQUIRE(observed_tool);
  CHECK(*observed_tool == scry_showcase::npc::NpcTool::look);
  CHECK(handler_thread == std::this_thread::get_id());
  CHECK(completion->text == "done");
  CHECK(world->position() == scry_showcase::npc::Position{.x = 2, .y = 2});

  REQUIRE(requests->requests().size() == 2);
  const auto& initial = requests->requests().front().body;
  for (const std::string_view name :
       {"look", "move_north", "move_south", "move_east", "move_west"}) {
    CHECK(initial.find(name) != std::string::npos);
  }
  CHECK(initial.find(R"("additionalProperties":false)") != std::string::npos);

  const auto& resend = requests->requests().back().body;
  CHECK(resend.find(R"("tool_call_id":"call-look")") != std::string::npos);
  CHECK(resend.find("available_moves") != std::string::npos);
}
