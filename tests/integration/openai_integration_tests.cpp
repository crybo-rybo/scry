#include "core/provider.hpp"
#include "runtime/test_access.hpp"
#include "support/transport/fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view openai_tool_stream =
    R"(data: {"id":"chatcmpl-tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant","tool_calls":[{"index":0,"id":"call-a","type":"function","function":{"name":"lookup","arguments":"{\"city\":"}}]},"finish_reason":null}]}

data: {"id":"chatcmpl-tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"Boston\"}"}}]},"finish_reason":null}]}

data: {"id":"chatcmpl-tools","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}

data: {"id":"chatcmpl-tools","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":4,"completion_tokens":3,"total_tokens":7}}

data: [DONE]

)";

constexpr std::string_view openai_final_stream =
    R"(data: {"id":"chatcmpl-final","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}

data: {"id":"chatcmpl-final","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"sunny"},"finish_reason":null}]}

data: {"id":"chatcmpl-final","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: {"id":"chatcmpl-final","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":9,"completion_tokens":2,"total_tokens":11}}

data: [DONE]

)";

constexpr std::string_view anthropic_final_stream = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg-final","type":"message","role":"assistant","content":[],"stop_reason":null,"usage":{"input_tokens":2,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"anthropic"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}}

event: message_stop
data: {"type":"message_stop"}

)";

[[nodiscard]] scry::Config openai_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1/v1/",
      .model = "local-model",
      .dialect = scry::ProviderDialect::openai_compatible,
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] scry::Config anthropic_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "anthropic-test-key",
      .model = "anthropic-model",
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] std::unique_ptr<scry::detail::ProviderAdapter>
provider(const scry::ProviderDialect dialect) {
  auto result = scry::detail::make_provider_adapter(dialect);
  REQUIRE(result);
  return std::move(*result);
}

[[nodiscard]] scry::test::ScriptedExchange
scripted_exchange(const std::string_view stream, std::string request_id) {
  std::vector<std::string> chunks;
  chunks.reserve(stream.size());
  for (const char byte : stream) {
    chunks.emplace_back(1, byte);
  }
  return {
      .body_chunks = std::move(chunks),
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

[[nodiscard]] scry::ToolDefinition lookup_tool() {
  return {
      .name = "lookup",
      .description = "Look up a city",
      .input_schema =
          {
              .text =
                  R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false})",
          },
  };
}

} // namespace

TEST_CASE("OpenAI-compatible config drives a fragmented transactional tool round") {
  auto fake = std::make_unique<scry::test::FakeTransport>();
  auto* requests = fake.get();
  fake->enqueue(scripted_exchange(openai_tool_stream, "openai-tool-request"));
  fake->enqueue(scripted_exchange(openai_final_stream, "openai-final-request"));
  auto created = scry::detail::HarnessTestAccess::create(
      openai_config(), provider(scry::ProviderDialect::openai_compatible),
      std::move(fake));
  REQUIRE(created);
  auto harness = std::move(*created);

  std::string arguments;
  std::thread::id handler_thread;
  REQUIRE(harness.tools().add(lookup_tool(),
                              [&](scry::Json value) -> scry::Result<scry::Json> {
                                arguments = std::move(value.text);
                                handler_thread = std::this_thread::get_id();
                                return scry::Json{.text = R"({"forecast":"sunny"})"};
                              }));
  auto conversation = scry::Conversation::create(
      {.system_prompt = "Use the lookup tool before answering."});
  REQUIRE(conversation);
  auto turn = harness.send(*conversation, "Weather in Boston?");
  REQUIRE(turn);

  std::vector<std::string> timeline;
  std::optional<scry::Completion> completion;
  REQUIRE(turn->on_tool_call(
      [&](const scry::ToolCall& call) { timeline.push_back("tool:" + call.name); }));
  REQUIRE(turn->on_completion([&](const scry::Completion& value) {
    timeline.emplace_back("complete");
    completion = value;
  }));
  REQUIRE(pump_until(harness, [&] { return completion.has_value(); }));

  CHECK(arguments == R"({"city":"Boston"})");
  CHECK(handler_thread == std::this_thread::get_id());
  CHECK(timeline == std::vector<std::string>{"tool:lookup", "complete"});
  CHECK(completion->text == "sunny");
  CHECK(completion->usage.input_tokens == 13);
  CHECK(completion->usage.output_tokens == 5);
  CHECK(completion->provider_request_id == "openai-final-request");
  CHECK(conversation->message_count() == 4);

  REQUIRE(requests->requests().size() == 2);
  for (const auto& request : requests->requests()) {
    CHECK(request.url == "http://127.0.0.1:1/v1/chat/completions");
    CHECK(request.body.find("anthropic") == std::string::npos);
    CHECK(request.body.find(R"("model":"local-model")") != std::string::npos);
  }
  const auto& initial = requests->requests().front();
  CHECK(initial.body.find(R"("role":"system")") != std::string::npos);
  CHECK(initial.body.find(R"("type":"function")") != std::string::npos);
  CHECK(initial.body.find(R"("include_usage":true)") != std::string::npos);
  const auto& resend = requests->requests().back().body;
  CHECK(resend.find(R"("role":"tool")") != std::string::npos);
  CHECK(resend.find(R"("tool_call_id":"call-a")") != std::string::npos);
  CHECK(resend.find(R"({\"forecast\":\"sunny\"})") != std::string::npos);
}

TEST_CASE("concurrent Harnesses keep Anthropic and OpenAI dialect state isolated") {
  auto anthropic_transport = std::make_unique<scry::test::FakeTransport>();
  auto* anthropic_requests = anthropic_transport.get();
  anthropic_transport->enqueue(
      scripted_exchange(anthropic_final_stream, "anthropic-request"));
  auto openai_transport = std::make_unique<scry::test::FakeTransport>();
  auto* openai_requests = openai_transport.get();
  openai_transport->enqueue(scripted_exchange(openai_final_stream, "openai-request"));

  auto anthropic = scry::detail::HarnessTestAccess::create(
      anthropic_config(), provider(scry::ProviderDialect::anthropic),
      std::move(anthropic_transport));
  auto openai = scry::detail::HarnessTestAccess::create(
      openai_config(), provider(scry::ProviderDialect::openai_compatible),
      std::move(openai_transport));
  REQUIRE(anthropic);
  REQUIRE(openai);
  auto anthropic_harness = std::move(*anthropic);
  auto openai_harness = std::move(*openai);

  auto anthropic_conversation = scry::Conversation::create();
  auto openai_conversation = scry::Conversation::create();
  REQUIRE(anthropic_conversation);
  REQUIRE(openai_conversation);
  auto anthropic_turn = anthropic_harness.send(*anthropic_conversation, "first");
  auto openai_turn = openai_harness.send(*openai_conversation, "second");
  REQUIRE(anthropic_turn);
  REQUIRE(openai_turn);

  std::optional<scry::Completion> anthropic_completion;
  std::optional<scry::Completion> openai_completion;
  REQUIRE(anthropic_turn->on_completion(
      [&](const scry::Completion& value) { anthropic_completion = value; }));
  REQUIRE(openai_turn->on_completion(
      [&](const scry::Completion& value) { openai_completion = value; }));
  for (std::size_t pump = 0;
       pump < 100'000 && (!anthropic_completion || !openai_completion); ++pump) {
    static_cast<void>(anthropic_harness.update());
    static_cast<void>(openai_harness.update());
    std::this_thread::yield();
  }

  REQUIRE(anthropic_completion);
  REQUIRE(openai_completion);
  CHECK(anthropic_completion->text == "anthropic");
  CHECK(openai_completion->text == "sunny");
  REQUIRE(anthropic_requests->requests().size() == 1);
  REQUIRE(openai_requests->requests().size() == 1);
  CHECK(anthropic_requests->requests().front().url.ends_with("/v1/messages"));
  CHECK(openai_requests->requests().front().url.ends_with("/v1/chat/completions"));
  CHECK(anthropic_requests->requests().front().body.find("stream_options") ==
        std::string::npos);
  CHECK(openai_requests->requests().front().body.find("stream_options") !=
        std::string::npos);
}
