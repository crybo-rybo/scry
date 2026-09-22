#include "core/json_codec.hpp"
#include "core/model.hpp"
#include "fixture_support.hpp"
#include "provider/openai.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;
using namespace scry::test_fixtures;

[[nodiscard]] ModelRequest request() {
  return ModelRequest{
      .system_prompt = "Be concise",
      .messages =
          {
              Message{
                  .role = Role::user,
                  .content =
                      {
                          TextBlock{.text = "Hel"},
                          TextBlock{.text = "lo"},
                      },
              },
              Message{
                  .role = Role::assistant,
                  .content =
                      {
                          TextBlock{.text = "Working"},
                          ToolCallBlock{
                              .id = "call-weather",
                              .name = "weather",
                              .arguments = Json{.text = R"({"city":"Paris"})"},
                          },
                      },
              },
              Message{
                  .role = Role::user,
                  .content =
                      {
                          ToolResultBlock{
                              .tool_call_id = "call-weather",
                              .result = Json{.text = R"({"temperature":21})"},
                          },
                          ToolResultBlock{
                              .tool_call_id = "call-other",
                              .result = Json{.text = R"({"error":"closed"})"},
                              .is_error = true,
                          },
                      },
              },
          },
      .tools = std::make_shared<const std::vector<ToolDefinition>>(
          std::vector<ToolDefinition>{
              ToolDefinition{
                  .name = "weather",
                  .description = "Get weather",
                  .input_schema =
                      Json{.text = R"({"type":"object","required":["city"]})"},
              },
          }),
      .sampling =
          SamplingConfig{
              .temperature = 1.5,
              .top_p = 0.0,
              .max_tokens = 64,
          },
  };
}

void require_invalid_request(const Config& value, const ModelRequest& model_request) {
  OpenAiAdapter adapter;
  const auto encoded = adapter.make_request(value, model_request);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == ErrorCategory::invalid_config);
  CHECK(encoded.error().message.find("sanitized-key") == std::string::npos);
}

} // namespace

TEST_CASE("OpenAI request is semantically equivalent to the common contract") {
  OpenAiAdapter adapter;
  const auto encoded = adapter.make_request(openai_config(), request());
  REQUIRE(encoded);
  CHECK(encoded->url == "https://api.openai.test/v1/chat/completions");
  // The transport prefixes body-derived error tokens with this namespace, so it
  // must match the one the stream decoder uses.
  CHECK(encoded->provider_namespace == "openai");
  CHECK(header(*encoded, "content-type") == "application/json");
  CHECK(header(*encoded, "accept") == "text/event-stream");
  CHECK(header(*encoded, "authorization") == "Bearer sanitized-key");
  CHECK(canonical(encoded->body) ==
        canonical(scry::test_fixtures::openai_fixture("request.json")));
  CHECK(encoded->body.find("is_error") == std::string::npos);
  CHECK(encoded->body.find("parallel_tool_calls") == std::string::npos);
  CHECK(encoded->body.find("reasoning_effort") == std::string::npos);
  CHECK(encoded->body.find("\"strict\"") == std::string::npos);
}

TEST_CASE("OpenAI request carries the configured network options") {
  const auto adapter = make_provider_adapter(ProviderDialect::openai_compatible);
  REQUIRE(adapter);

  auto networked = openai_config();
  networked.extra_headers = {HttpHeader{.name = "x-scry-example", .value = "1"}};
  networked.ca_bundle_path = "/tmp/ca.pem";
  networked.proxy = "http://proxy.internal:3128";

  const auto encoded = adapter->make_request(networked, request());
  REQUIRE(encoded.has_value());
  CHECK(encoded->ca_bundle_path == "/tmp/ca.pem");
  CHECK(encoded->proxy == "http://proxy.internal:3128");
  REQUIRE(encoded->headers.size() == 4);
  CHECK(encoded->headers.back().name == "x-scry-example");
  CHECK(encoded->headers.back().value == "1");
  CHECK(header(*encoded, "authorization") == "Bearer sanitized-key");
}

TEST_CASE("OpenAI request omits max_tokens when the sampling value is unset") {
  OpenAiAdapter adapter;
  auto model_request = request();
  model_request.sampling.max_tokens.reset();

  const auto encoded = adapter.make_request(openai_config(), model_request);
  REQUIRE(encoded);
  CHECK(encoded->body.find("max_tokens") == std::string::npos);
  CHECK(encoded->body.find(R"("temperature":1.5)") != std::string::npos);
}

TEST_CASE("OpenAI request can disable reasoning without changing the default") {
  OpenAiAdapter adapter;
  auto no_reasoning = openai_config();
  no_reasoning.reasoning_mode = ReasoningMode::disabled;

  const auto encoded = adapter.make_request(no_reasoning, request());
  REQUIRE(encoded);
  CHECK(encoded->body.find(R"("reasoning_effort":"none")") != std::string::npos);
}

TEST_CASE("OpenAI endpoint normalization accepts only the documented base forms") {
  OpenAiAdapter adapter;
  constexpr std::array cases{
      std::pair{"https://example.test", "https://example.test/v1/chat/completions"},
      std::pair{"https://example.test/", "https://example.test/v1/chat/completions"},
      std::pair{"https://example.test/proxy/v1",
                "https://example.test/proxy/v1/chat/completions"},
      std::pair{"https://example.test/proxy/v1/",
                "https://example.test/proxy/v1/chat/completions"},
      std::pair{"https://example.test/proxy/v1/chat/completions/",
                "https://example.test/proxy/v1/chat/completions"},
      std::pair{"https://example.test/chat/completions",
                "https://example.test/chat/completions/v1/chat/completions"},
  };
  const auto model_request = request();
  for (const auto& [base, expected] : cases) {
    INFO(base);
    const auto encoded = adapter.make_request(openai_config(base), model_request);
    REQUIRE(encoded);
    CHECK(encoded->url == expected);
    CHECK(header(*encoded, "accept") == "text/event-stream");
  }
}

TEST_CASE("OpenAI authentication is optional for local servers") {
  OpenAiAdapter adapter;
  auto local = openai_config("http://localhost:11434/v1");
  local.api_key.clear();
  const auto encoded = adapter.make_request(local, request());
  REQUIRE(encoded);
  CHECK(header(*encoded, "authorization").empty());
}

TEST_CASE("OpenAI request preserves assistant text-only and tool-only shapes") {
  OpenAiAdapter adapter;
  auto model_request = request();
  model_request.system_prompt.clear();
  model_request.messages = {
      Message{.role = Role::assistant,
              .content = {TextBlock{.text = "plain response"}}},
      Message{.role = Role::assistant,
              .content = {ToolCallBlock{
                  .id = "call",
                  .name = "lookup",
                  .arguments = Json{.text = "{}"},
              }}},
  };
  model_request.tools.reset();
  model_request.sampling.top_p.reset();

  const auto encoded = adapter.make_request(openai_config(), model_request);
  REQUIRE(encoded);
  CHECK(encoded->body.find(R"("content":"plain response")") != std::string::npos);
  CHECK(encoded->body.find(R"("content":null)") != std::string::npos);
  CHECK(encoded->body.find(R"("top_p")") == std::string::npos);
  CHECK(encoded->body.find(R"("tools")") == std::string::npos);
}

// Embedded JSON payloads are covered by the matrix in request_encoding_tests.cpp;
// these are the neutral shapes the Chat Completions wire cannot carry.
TEST_CASE("OpenAI request rejects malformed tool boundary fields") {
  auto invalid = request();
  std::get<ToolCallBlock>(invalid.messages[1].content[1]).id.clear();
  require_invalid_request(openai_config(), invalid);

  invalid = request();
  std::get<ToolCallBlock>(invalid.messages[1].content[1]).name.clear();
  require_invalid_request(openai_config(), invalid);

  invalid = request();
  std::get<ToolResultBlock>(invalid.messages[2].content[0]).tool_call_id.clear();
  require_invalid_request(openai_config(), invalid);

  invalid = request();
  invalid.messages[1].content = {
      ToolResultBlock{.tool_call_id = "call", .result = Json{.text = "{}"}}};
  require_invalid_request(openai_config(), invalid);

  invalid = request();
  invalid.messages.front().content.push_back(
      ToolResultBlock{.tool_call_id = "call", .result = Json{.text = "{}"}});
  require_invalid_request(openai_config(), invalid);

  invalid = request();
  invalid.messages.front().content = {
      TextBlock{},
      ToolResultBlock{.tool_call_id = "call", .result = Json{.text = "{}"}},
  };
  require_invalid_request(openai_config(), invalid);

  invalid = request();
  invalid.tools = std::make_shared<const std::vector<ToolDefinition>>(
      std::vector<ToolDefinition>{{.name = "",
                                   .description = "lookup",
                                   .input_schema = {.text = R"({"type":"object"})"}}});
  require_invalid_request(openai_config(), invalid);
}

TEST_CASE("OpenAI request leaves a history ending in tool results unmerged") {
  // The Anthropic adapter merges consecutive same-role messages so a turn stopped
  // at the tool-round limit stays encodable. This dialect needs no such rule: tool
  // results already become their own `tool` messages, so a `user` message may
  // follow them directly.
  OpenAiAdapter adapter;
  auto model_request = request();
  model_request.history =
      std::make_shared<const std::vector<Message>>(
          std::vector<Message>{
              Message{.role = Role::user,
                      .content = {TextBlock{.text = "first question"}}},
              Message{.role = Role::assistant,
                      .content =
                          {
                              ToolCallBlock{
                                  .id = "tool_1",
                                  .name = "lookup",
                                  .arguments = Json{.text = R"({"key":"value"})"},
                              },
                          }},
              Message{.role = Role::user,
                      .content =
                          {
                              ToolResultBlock{
                                  .tool_call_id = "tool_1",
                                  .result = Json{.text = R"({"answer":42})"},
                              },
                          }},
          });
  model_request.messages = {
      Message{.role = Role::user, .content = {TextBlock{.text = "second question"}}},
  };

  const auto encoded = adapter.make_request(openai_config(), model_request);
  REQUIRE(encoded);
  const auto body =
      parse_json(encoded->body, ErrorCategory::protocol, "body is not valid JSON");
  REQUIRE(body);
  const auto messages = required_json_array(*body, "messages");
  REQUIRE(messages);
  REQUIRE((*messages)->size() == 5);
  const std::array expected{"system", "user", "assistant", "tool", "user"};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto role = required_json_string((**messages)[index], "role");
    REQUIRE(role);
    CHECK(*role == expected[index]);
  }
}
