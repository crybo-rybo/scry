#include "core/json_codec.hpp"
#include "core/model.hpp"
#include "provider/openai.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] std::string canonical(const std::string_view json) {
  // Request fixtures assert JSON meaning. They deliberately do not promise
  // byte-for-byte wire spelling or member order.
  auto parsed = parse_json(json, ErrorCategory::protocol, "test JSON is invalid");
  REQUIRE(parsed);
  auto encoded = write_json_text(*parsed, ErrorCategory::protocol,
                                 "test JSON could not be encoded");
  REQUIRE(encoded);
  return *encoded;
}

[[nodiscard]] std::string header(const TransportRequest& request,
                                 const std::string_view name) {
  for (const auto& value : request.headers) {
    if (value.name == name) {
      return value.value;
    }
  }
  return {};
}

[[nodiscard]] Config config(std::string base_url = "https://api.openai.test/v1") {
  return Config{
      .base_url = std::move(base_url),
      .api_key = "sanitized-key",
      .model = "chat-model",
      .dialect = ProviderDialect::openai_compatible,
  };
}

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
      .tools = share_tool_schemas({
          ToolSchema{
              .name = "weather",
              .description = "Get weather",
              .input_schema = Json{.text = R"({"type":"object","required":["city"]})"},
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

void replace_tools(ModelRequest& request, std::vector<ToolSchema> schemas) {
  request.tools = share_tool_schemas(std::move(schemas));
}

} // namespace

TEST_CASE("OpenAI request is semantically equivalent to the common contract") {
  OpenAiAdapter adapter;
  const auto encoded = adapter.make_request(config(), request());
  REQUIRE(encoded);
  CHECK(encoded->url == "https://api.openai.test/v1/chat/completions");
  CHECK(header(*encoded, "content-type") == "application/json");
  CHECK(header(*encoded, "accept") == "text/event-stream");
  CHECK(header(*encoded, "authorization") == "Bearer sanitized-key");
  CHECK(
      canonical(encoded->body) ==
      canonical(
          R"({"model":"chat-model","messages":[{"role":"system","content":"Be concise"},{"role":"user","content":"Hello"},{"role":"assistant","content":"Working","tool_calls":[{"id":"call-weather","type":"function","function":{"name":"weather","arguments":"{\"city\":\"Paris\"}"}}]},{"role":"tool","tool_call_id":"call-weather","content":"{\"temperature\":21}"},{"role":"tool","tool_call_id":"call-other","content":"{\"error\":\"closed\"}"}],"temperature":1.5,"max_tokens":64,"stream":true,"top_p":0.0,"stream_options":{"include_usage":true},"tools":[{"type":"function","function":{"name":"weather","description":"Get weather","parameters":{"type":"object","required":["city"]}}}]})"));
  CHECK(encoded->body.find("is_error") == std::string::npos);
  CHECK(encoded->body.find("parallel_tool_calls") == std::string::npos);
  CHECK(encoded->body.find("reasoning_effort") == std::string::npos);
  CHECK(encoded->body.find("\"strict\"") == std::string::npos);
}

TEST_CASE("OpenAI request can disable reasoning without changing the default") {
  OpenAiAdapter adapter;
  auto no_reasoning = config();
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
    const auto encoded = adapter.make_request(config(base), model_request);
    REQUIRE(encoded);
    CHECK(encoded->url == expected);
    CHECK(header(*encoded, "accept") == "text/event-stream");
  }
}

TEST_CASE("OpenAI authentication is optional for local servers") {
  OpenAiAdapter adapter;
  auto local = config("http://localhost:11434/v1");
  local.api_key.clear();
  const auto encoded = adapter.make_request(local, request());
  REQUIRE(encoded);
  CHECK(header(*encoded, "authorization").empty());
}

TEST_CASE("OpenAI request rejects neutral shapes that cannot be preserved") {
  OpenAiAdapter adapter;
  auto invalid = request();
  invalid.messages.front().content.push_back(ToolResultBlock{
      .tool_call_id = "call",
      .result = Json{.text = "{}"},
  });
  auto encoded = adapter.make_request(config(), invalid);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == ErrorCategory::invalid_config);

  invalid = request();
  invalid.messages.front().content = {
      TextBlock{},
      ToolResultBlock{
          .tool_call_id = "call",
          .result = Json{.text = "{}"},
      },
  };
  encoded = adapter.make_request(config(), invalid);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == ErrorCategory::invalid_config);

  invalid = request();
  std::get<ToolCallBlock>(invalid.messages[1].content[1]).arguments.text = "[]";
  encoded = adapter.make_request(config(), invalid);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == ErrorCategory::invalid_config);

  invalid = request();
  auto invalid_schema = tool_schemas(invalid);
  invalid_schema.front().input_schema.text = "[]";
  replace_tools(invalid, std::move(invalid_schema));
  encoded = adapter.make_request(config(), invalid);
  REQUIRE_FALSE(encoded);
  CHECK(encoded.error().category == ErrorCategory::invalid_config);
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

  const auto encoded = adapter.make_request(config(), model_request);
  REQUIRE(encoded);
  CHECK(encoded->body.find(R"("content":"plain response")") != std::string::npos);
  CHECK(encoded->body.find(R"("content":null)") != std::string::npos);
  CHECK(encoded->body.find(R"("top_p")") == std::string::npos);
  CHECK(encoded->body.find(R"("tools")") == std::string::npos);
}

TEST_CASE("OpenAI request rejects malformed tool boundary fields") {
  auto invalid = request();
  std::get<ToolCallBlock>(invalid.messages[1].content[1]).id.clear();
  require_invalid_request(config(), invalid);

  invalid = request();
  std::get<ToolCallBlock>(invalid.messages[1].content[1]).name.clear();
  require_invalid_request(config(), invalid);

  invalid = request();
  std::get<ToolCallBlock>(invalid.messages[1].content[1]).arguments.text = "{";
  require_invalid_request(config(), invalid);

  invalid = request();
  std::get<ToolResultBlock>(invalid.messages[2].content[0]).tool_call_id.clear();
  require_invalid_request(config(), invalid);

  invalid = request();
  std::get<ToolResultBlock>(invalid.messages[2].content[0]).result.text = "{";
  require_invalid_request(config(), invalid);

  invalid = request();
  invalid.messages[1].content = {
      ToolResultBlock{.tool_call_id = "call", .result = Json{.text = "{}"}}};
  require_invalid_request(config(), invalid);

  invalid = request();
  auto nameless = tool_schemas(invalid);
  nameless.front().name.clear();
  replace_tools(invalid, std::move(nameless));
  require_invalid_request(config(), invalid);

  invalid = request();
  auto malformed = tool_schemas(invalid);
  malformed.front().input_schema.text = "{";
  replace_tools(invalid, std::move(malformed));
  require_invalid_request(config(), invalid);
}
