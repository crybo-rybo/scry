#include "support/harness_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <scry/reflection.hpp>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace scry::test_support;

namespace {

constexpr std::string_view openai_tool_call_fixture =
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

const std::string anthropic_final_stream =
    anthropic_text_stream("anthropic", "msg-final", {}, 2, 1);

[[nodiscard]] scry::Config openai_config() {
  auto config = test_config();
  config.api_key.clear();
  config.model = "local-model";
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

[[nodiscard]] scry::Config anthropic_config() {
  auto config = test_config();
  config.api_key = "anthropic-test-key";
  config.model = "anthropic-model";
  return config;
}

// Splitting the stream into single-byte chunks proves the decoder reassembles
// frames that arrive without regard for event boundaries.
[[nodiscard]] ScriptedResponse byte_chunked_response(const std::string_view stream,
                                                     std::string request_id) {
  ScriptedResponse response{
      .headers = {{.name = "request-id", .value = std::move(request_id)}},
  };
  response.body_chunks.reserve(stream.size());
  for (const char byte : stream) {
    response.body_chunks.emplace_back(1, byte);
  }
  return response;
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

enum class Direction {
  north,
  south,
};

struct MoveArguments {
  Direction direction{};
};

struct MoveResult {
  std::string outcome{};
};

// One scripted tool round followed by a plain text turn, which is the shortest
// script that puts a tool result on the wire.
[[nodiscard]] HarnessFixture move_fixture(std::string_view call) {
  return make_harness_fixture(
      openai_config(),
      {
          scripted_response(
              openai_tool_stream({{.id = "call-a", .name = "move", .arguments = call}}),
              "openai-tool-request"),
          scripted_response(openai_text_stream("done"), "openai-final-request"),
      });
}

[[nodiscard]] std::string run_move_turn(HarnessFixture& fixture) {
  std::optional<scry::Completion> completion;
  const auto turn = fixture.harness.send(
      fixture.conversation, "Move north.",
      {
          .on_finished =
              [&completion](scry::Result<scry::Completion> finished) {
                REQUIRE(finished);
                completion = std::move(*finished);
              },
      });
  REQUIRE(turn);
  REQUIRE(pump_until(fixture.harness, [&] { return completion.has_value(); }));
  const auto recorded = fixture.server.requests();
  REQUIRE(recorded.size() == 2);
  return recorded.back().body;
}

} // namespace

TEST_CASE("a handler refusal reaches the model in the OpenAI tool message") {
  auto fixture = move_fixture(R"({"direction":"north"})");
  REQUIRE(fixture.harness.tools().add<MoveArguments>(
      {.name = "move", .description = "Move the player one square"},
      [](MoveArguments) -> scry::Result<MoveResult> {
        return std::unexpected(scry::tool_error("a wall blocks the way north",
                                                "secret application message"));
      }));

  const auto resend = run_move_turn(fixture);

  CHECK(resend.find(R"("role":"tool")") != std::string::npos);
  CHECK(resend.find(R"({\"error\":\"a wall blocks the way north\"})") !=
        std::string::npos);
  CHECK(resend.find("secret") == std::string::npos);
}

TEST_CASE("an invalid enum argument round-trips into the OpenAI tool message") {
  auto fixture = move_fixture(R"({"direction":"up"})");
  REQUIRE(fixture.harness.tools().add<MoveArguments>(
      {.name = "move", .description = "Move the player one square"},
      [](MoveArguments) -> scry::Result<MoveResult> {
        FAIL("the handler must not run when arguments do not decode");
        return MoveResult{};
      }));

  const auto resend = run_move_turn(fixture);

  CHECK(
      resend.find(
          R"($.direction is not a declared enumerator; must be one of: north, south)") !=
      std::string::npos);
  CHECK(resend.find("reflected JSON at") == std::string::npos);
}

TEST_CASE("OpenAI-compatible config drives a fragmented transactional tool round") {
  auto server = start_server();
  server.enqueue(
      byte_chunked_response(openai_tool_call_fixture, "openai-tool-request"));
  server.enqueue(byte_chunked_response(openai_final_stream, "openai-final-request"));
  auto config = openai_config();
  // The endpoint path is appended to a base URL that already ends in /v1/.
  config.base_url = server.url() + "/v1/";
  auto harness = unwrap(scry::Harness::create(config));

  std::string arguments;
  std::thread::id handler_thread;
  REQUIRE(harness.tools().add_dynamic(
      lookup_tool(), [&](scry::Json value) -> scry::Result<scry::Json> {
        arguments = std::move(value.text);
        handler_thread = std::this_thread::get_id();
        return scry::Json{.text = R"({"forecast":"sunny"})"};
      }));
  auto conversation = scry::Conversation::create(
      {.system_prompt = "Use the lookup tool before answering."});
  REQUIRE(conversation);
  std::vector<std::string> timeline;
  std::optional<scry::Completion> completion;
  auto turn = harness.send(*conversation, "Weather in Boston?",
                           {
                               .on_tool_call =
                                   [&](const scry::ToolCall& call) {
                                     timeline.push_back("tool:" + call.name);
                                   },
                               .on_finished =
                                   [&](scry::Result<scry::Completion> finished) {
                                     REQUIRE(finished);
                                     timeline.emplace_back("complete");
                                     completion = std::move(*finished);
                                   },
                           });
  REQUIRE(turn);
  REQUIRE(pump_until(harness, [&] { return completion.has_value(); }));

  CHECK(arguments == R"({"city":"Boston"})");
  CHECK(handler_thread == std::this_thread::get_id());
  CHECK(timeline == std::vector<std::string>{"tool:lookup", "complete"});
  CHECK(completion->text == "sunny");
  CHECK(completion->usage.input_tokens == 13);
  CHECK(completion->usage.output_tokens == 5);
  CHECK(completion->provider_request_id == "openai-final-request");
  CHECK(conversation->message_count() == 4);

  const auto recorded = server.requests();
  REQUIRE(recorded.size() == 2);
  for (const auto& request : recorded) {
    CHECK(request.target == "/v1/chat/completions");
    CHECK(request.body.find("anthropic") == std::string::npos);
    CHECK(request.body.find(R"("model":"local-model")") != std::string::npos);
  }
  const auto& initial = recorded.front();
  CHECK(initial.body.find(R"("role":"system")") != std::string::npos);
  CHECK(initial.body.find(R"("type":"function")") != std::string::npos);
  CHECK(initial.body.find(R"("include_usage":true)") != std::string::npos);
  const auto& resend = recorded.back().body;
  CHECK(resend.find(R"("role":"tool")") != std::string::npos);
  CHECK(resend.find(R"("tool_call_id":"call-a")") != std::string::npos);
  CHECK(resend.find(R"({\"forecast\":\"sunny\"})") != std::string::npos);
}

TEST_CASE("concurrent Harnesses keep Anthropic and OpenAI dialect state isolated") {
  auto anthropic = make_harness_fixture(
      anthropic_config(),
      {byte_chunked_response(anthropic_final_stream, "anthropic-request")});
  auto openai = make_harness_fixture(
      openai_config(), {byte_chunked_response(openai_final_stream, "openai-request")});

  std::optional<scry::Completion> anthropic_completion;
  std::optional<scry::Completion> openai_completion;
  const auto capture = [](std::optional<scry::Completion>& target) {
    return scry::TurnCallbacks{
        .on_finished =
            [&target](scry::Result<scry::Completion> finished) {
              REQUIRE(finished);
              target = std::move(*finished);
            },
    };
  };
  auto anthropic_turn = anthropic.harness.send(anthropic.conversation, "first",
                                               capture(anthropic_completion));
  auto openai_turn =
      openai.harness.send(openai.conversation, "second", capture(openai_completion));
  REQUIRE(anthropic_turn);
  REQUIRE(openai_turn);

  // Both workers run at once; each Harness delivers only its own turn.
  REQUIRE(pump_until(anthropic.harness, [&anthropic_completion] {
    return anthropic_completion.has_value();
  }));
  REQUIRE(pump_until(openai.harness,
                     [&openai_completion] { return openai_completion.has_value(); }));

  CHECK(anthropic_completion->text == "anthropic");
  CHECK(openai_completion->text == "sunny");
  const auto anthropic_recorded = anthropic.server.requests();
  const auto openai_recorded = openai.server.requests();
  REQUIRE(anthropic_recorded.size() == 1);
  REQUIRE(openai_recorded.size() == 1);
  CHECK(anthropic_recorded.front().target == "/v1/messages");
  CHECK(openai_recorded.front().target == "/v1/chat/completions");
  CHECK(anthropic_recorded.front().body.find("stream_options") == std::string::npos);
  CHECK(openai_recorded.front().body.find("stream_options") != std::string::npos);
}
