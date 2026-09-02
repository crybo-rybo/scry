#include "runtime/test_access.hpp"
#include "support/harness_test_support.hpp"
#include "world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace scry::test_support;

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

// The showcase talks to a local OpenAI-compatible server, so it needs its own
// endpoint and dialect rather than the shared Anthropic defaults.
[[nodiscard]] scry::Config showcase_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1/v1",
      .model = "showcase-test-model",
      .dialect = scry::ProviderDialect::openai_compatible,
      .reasoning_mode = scry::ReasoningMode::disabled,
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

} // namespace

TEST_CASE("NPC registrations execute on the update thread and resend observations") {
  auto transport = std::make_unique<scry::test::FakeTransport>();
  auto* requests = transport.get();
  transport->enqueue(scripted_exchange(tool_stream, "npc-tool-request"));
  transport->enqueue(scripted_exchange(final_stream, "npc-final-request"));
  auto created = scry::detail::HarnessTestAccess::create(
      showcase_config(), provider(scry::ProviderDialect::openai_compatible),
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
  std::optional<scry::Completion> completion;
  std::optional<scry::Error> error;
  auto turn = harness.send(*conversation, "Look before moving.",
                           {
                               .on_finished =
                                   [&](scry::Result<scry::Completion> finished) {
                                     if (finished) {
                                       completion = std::move(*finished);
                                     } else {
                                       error = std::move(finished.error());
                                     }
                                   },
                           });
  REQUIRE(turn);

  REQUIRE(
      pump_until(harness, [&] { return completion.has_value() || error.has_value(); }));

  REQUIRE_FALSE(error);
  REQUIRE(completion);
  REQUIRE(observed_tool);
  CHECK(*observed_tool == scry_showcase::npc::NpcTool::look);
  CHECK(handler_thread == std::this_thread::get_id());
  CHECK(completion->text == "done");
  CHECK(world->position() == scry_showcase::npc::Position{.x = 2, .y = 2});

  const auto recorded = requests->requests();
  REQUIRE(recorded.size() == 2);
  const auto& initial = recorded.front().body;
  for (const std::string_view name :
       {"look", "move_north", "move_south", "move_east", "move_west"}) {
    CHECK(initial.find(name) != std::string::npos);
  }
  CHECK(initial.find(R"("additionalProperties":false)") != std::string::npos);
  CHECK(initial.find(R"("reasoning_effort":"none")") != std::string::npos);

  const auto& resend = recorded.back().body;
  CHECK(resend.find(R"("tool_call_id":"call-look")") != std::string::npos);
  CHECK(resend.find("available_moves") != std::string::npos);
}
