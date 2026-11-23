#include "core/model.hpp"
#include "core/provider.hpp"
#include "fixture_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;
using namespace scry::test_fixtures;

// Every embedded payload below is spelled exactly as Scry's own codec would
// have written it, because that is what reaches an adapter in production: the
// turn machine canonicalizes tool-call arguments, dispatch canonicalizes
// results, and registration canonicalizes schemas.
constexpr auto weather_arguments =
    std::string_view{R"({"city":"Paris","options":{"units":"metric","verbose":true}})"};
constexpr auto almanac_arguments =
    std::string_view{R"({"query":{"city":"Berlin","days":[1,2,3]}})"};
constexpr auto weather_result =
    std::string_view{R"({"city":"Paris","forecast":{"high":21,"low":11}})"};
constexpr auto almanac_result = std::string_view{R"({"error":"almanac unavailable"})"};
constexpr auto weather_schema = std::string_view{
    R"({"properties":{"city":{"type":"string"},"options":{"properties":{"units":{"type":"string"}},"type":"object"}},"required":["city"],"type":"object"})"};
constexpr auto almanac_schema = std::string_view{
    R"({"properties":{"query":{"properties":{"days":{"items":{"type":"integer"},"type":"array"}},"type":"object"}},"required":["query"],"type":"object"})"};

// Text in two blocks with bytes that need escaping, an assistant turn with two
// nested-object tool calls, one plain and one error tool result, a following user
// message, and two tools with nested schemas.
[[nodiscard]] ModelRequest tool_history_request() {
  return ModelRequest{
      .system_prompt = "Be concise",
      .history = std::make_shared<const std::vector<Message>>(std::vector<Message>{
          Message{.role = Role::user,
                  .content =
                      {
                          TextBlock{.text = "Weather in "},
                          TextBlock{.text = "\"Paris\" caf\xC3\xA9?\x1b[0m\n"},
                      }},
          Message{
              .role = Role::assistant,
              .content =
                  {
                      TextBlock{.text = "Checking."},
                      ToolCallBlock{
                          .id = "call-a",
                          .name = "weather",
                          .arguments =
                              scrydantic::Json{.text = std::string{weather_arguments}},
                      },
                      ToolCallBlock{
                          .id = "call-b",
                          .name = "almanac",
                          .arguments =
                              scrydantic::Json{.text = std::string{almanac_arguments}},
                      },
                  }},
          Message{.role = Role::user,
                  .content =
                      {
                          ToolResultBlock{
                              .tool_call_id = "call-a",
                              .result =
                                  scrydantic::Json{.text = std::string{weather_result}},
                          },
                          ToolResultBlock{
                              .tool_call_id = "call-b",
                              .result =
                                  scrydantic::Json{.text = std::string{almanac_result}},
                              .is_error = true,
                          },
                      }},
      }),
      .messages =
          {
              Message{.role = Role::user,
                      .content = {TextBlock{.text = "And tomorrow?"}}},
          },
      .tools = std::make_shared<const std::vector<ToolDefinition>>(
          std::vector<ToolDefinition>{
              ToolDefinition{
                  .name = "weather",
                  .description = "Get weather",
                  .input_schema = scrydantic::Json{.text = std::string{weather_schema}},
              },
              ToolDefinition{
                  .name = "almanac",
                  .description = "",
                  .input_schema = scrydantic::Json{.text = std::string{almanac_schema}},
              },
          }),
      .sampling =
          SamplingConfig{
              .temperature = 0.25,
              .top_p = 0.0,
              .max_tokens = 64,
          },
  };
}

[[nodiscard]] std::string encoded_body(const Config& config,
                                       const ModelRequest& request) {
  const auto adapter = make_provider_adapter(config.dialect);
  REQUIRE(adapter);
  const auto encoded = adapter->make_request(config, request);
  REQUIRE(encoded);
  return encoded->body;
}

} // namespace

TEST_CASE("Anthropic request over a tool history matches the encoder golden") {
  const auto body = encoded_body(anthropic_config(), tool_history_request());
  CHECK(canonical(body) ==
        canonical(scry::test_fixtures::anthropic_fixture("request_tool_history.json")));
}

TEST_CASE("OpenAI request over a tool history matches the encoder golden") {
  const auto body = encoded_body(openai_config(), tool_history_request());
  CHECK(canonical(body) ==
        canonical(scry::test_fixtures::openai_fixture("request_tool_history.json")));
}

TEST_CASE("request encoders omit the options a request leaves unset") {
  const ModelRequest request{
      .messages = {Message{.role = Role::user, .content = {TextBlock{}}}},
      .tools = std::make_shared<const std::vector<ToolDefinition>>(),
      .sampling = SamplingConfig{.max_tokens = 16},
  };
  CHECK(
      canonical(encoded_body(anthropic_config(), request)) ==
      canonical(
          R"({"max_tokens":16,"messages":[{"content":[{"text":"","type":"text"}],"role":"user"}],"model":"claude-test","stream":true,"temperature":1})"));
  CHECK(
      canonical(encoded_body(openai_config(), request)) ==
      canonical(
          R"({"max_tokens":16,"messages":[{"content":"","role":"user"}],"model":"chat-model","stream":true,"stream_options":{"include_usage":true},"temperature":1})"));
}

// A typed turn's request, which the goldens above do not carry.
TEST_CASE("request encoders offer a response tool last and require a tool call") {
  auto request = tool_history_request();
  request.response_tool = std::make_shared<const ToolDefinition>(ToolDefinition{
      .name = "respond",
      .description = "Give the \"final\" answer",
      .input_schema = scrydantic::Json{.text = R"({"type":"object"})"},
  });

  const auto anthropic = json_view(encoded_body(anthropic_config(), request));
  CHECK(anthropic.find("tool_choice")->to_json().text == R"({"type":"any"})");
  REQUIRE(anthropic.find("tools")->size() == 3);
  CHECK(
      anthropic.find("tools")->at(2)->to_json().text ==
      R"({"description":"Give the \"final\" answer","input_schema":{"type":"object"},"name":"respond"})");

  const auto openai = json_view(encoded_body(openai_config(), request));
  CHECK(openai.find("tool_choice")->to_json().text == R"("required")");
  REQUIRE(openai.find("tools")->size() == 3);
  CHECK(
      openai.find("tools")->at(2)->to_json().text ==
      R"({"function":{"description":"Give the \"final\" answer","name":"respond","parameters":{"type":"object"}},"type":"function"})");
}

namespace {

// A payload placed where each adapter embeds one, so the rejection contract can
// be asserted per field: schemas and arguments must be objects, results any JSON.
enum class Embedded { schema, arguments, result };

[[nodiscard]] ModelRequest request_embedding(const Embedded where,
                                             const std::string_view text) {
  ModelRequest request{.sampling = SamplingConfig{.max_tokens = 64}};
  auto schema = std::string{R"({"type":"object"})"};
  auto arguments = std::string{R"({"a":1})"};
  auto result = std::string{R"({"ok":true})"};
  (where == Embedded::schema      ? schema
   : where == Embedded::arguments ? arguments
                                  : result) = std::string{text};
  request.tools = std::make_shared<const std::vector<ToolDefinition>>(
      std::vector<ToolDefinition>{ToolDefinition{
          .name = "lookup",
          .description = "Look a value up",
          .input_schema = scrydantic::Json{.text = std::move(schema)},
      }});
  request.messages = {
      Message{.role = Role::user, .content = {TextBlock{.text = "go"}}},
      Message{.role = Role::assistant,
              .content = {ToolCallBlock{
                  .id = "call-a",
                  .name = "lookup",
                  .arguments = scrydantic::Json{.text = std::move(arguments)}}}},
      Message{.role = Role::user,
              .content = {ToolResultBlock{
                  .tool_call_id = "call-a",
                  .result = scrydantic::Json{.text = std::move(result)}}}},
  };
  return request;
}

[[nodiscard]] bool encodes(const Config& config, const ModelRequest& request) {
  const auto adapter = make_provider_adapter(config.dialect);
  REQUIRE(adapter);
  const auto encoded = adapter->make_request(config, request);
  if (!encoded) {
    CHECK(encoded.error().category == ErrorCategory::invalid_config);
    CHECK(encoded.error().message.find(config.api_key) == std::string::npos);
    return false;
  }
  // Whatever was spliced, the body itself must still be one JSON document.
  CHECK(scrydantic::JsonView::parse(scrydantic::Json{.text = encoded->body}));
  return true;
}

struct EmbeddedCase {
  std::string_view name{};
  std::string_view text{};
  bool object_accepted{};
  bool value_accepted{};
};

} // namespace

// The encoders are the last check in front of the wire for a ModelRequest
// assembled by hand, so the splice is preceded by a validation of every byte of
// each embedded payload, not a look at its first and last characters.
TEST_CASE("request encoders validate embedded JSON before splicing it") {
  static constexpr EmbeddedCase cases[] = {
      {"malformed interior", R"({"x":})", false, false},
      {"not JSON at all", "not-json", false, false},
      {"sibling injection", R"({} , "injected": true, "unused": {})", false, false},
      {"truncated object", R"({"a":1)", false, false},
      {"trailing garbage", R"({"a":1} x)", false, false},
      {"second document", R"({"a":1}{"b":2})", false, false},
      {"empty", "", false, false},
      {"whitespace only", "  \n", false, false},
      {"array root", "[1]", false, true},
      {"scalar root", R"("abc")", false, true},
      {"object with surrounding whitespace", " {\"a\":1} \n", true, true},
      {"canonical object", R"({"a":1})", true, true},
  };
  for (const auto& config : {anthropic_config(), openai_config()}) {
    for (const auto& item : cases) {
      INFO(config.model << ": " << item.name);
      CHECK(encodes(config, request_embedding(Embedded::schema, item.text)) ==
            item.object_accepted);
      CHECK(encodes(config, request_embedding(Embedded::arguments, item.text)) ==
            item.object_accepted);
      CHECK(encodes(config, request_embedding(Embedded::result, item.text)) ==
            item.value_accepted);
    }
  }
}

namespace {

[[nodiscard]] ModelRequest text_request(std::string text) {
  return ModelRequest{
      .messages = {Message{.role = Role::user,
                           .content = {TextBlock{.text = std::move(text)}}}},
      .sampling = SamplingConfig{.max_tokens = 16},
  };
}

} // namespace

TEST_CASE("request encoders reject text that is not UTF-8 instead of sending it") {
  // A body holding such a string would not be JSON, so it fails before transport.
  for (const auto& config : {anthropic_config(), openai_config()}) {
    INFO(config.model);
    const auto adapter = make_provider_adapter(config.dialect);
    const auto encoded = adapter->make_request(config, text_request("caf\xFF"));
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().category == ErrorCategory::invalid_config);
    CHECK(encoded.error().message.ends_with(
        "request: the value holds a string that is not valid UTF-8, so it cannot be "
        "JSON"));
  }
}

TEST_CASE("the deepest tool arguments the turn accepts fit an Anthropic body") {
  // TurnMachine accepts arguments 251 deep; the body holds them five levels down.
  auto request = text_request("t");
  request.messages.push_back(Message{
      .role = Role::assistant,
      .content = {ToolCallBlock{
          .id = "call-1",
          .name = "tool",
          .arguments = scrydantic::Json{.text = "{\"a\":" + std::string(250, '[') +
                                                std::string(250, ']') + "}"},
      }},
  });
  const auto config = anthropic_config();
  const auto adapter = make_provider_adapter(config.dialect);
  CHECK(adapter->make_request(config, request));
}
