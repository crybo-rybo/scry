#include "core/json_codec.hpp"
#include "core/model.hpp"
#include "core/provider.hpp"
#include "fixture_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] std::string canonical(const std::string_view json) {
  auto parsed = parse_json(json, ErrorCategory::protocol, "test JSON is invalid");
  REQUIRE(parsed);
  auto encoded = write_json_text(*parsed, ErrorCategory::protocol,
                                 "test JSON could not be encoded");
  REQUIRE(encoded);
  return *encoded;
}

// Every embedded payload below is spelled exactly as Scry's own codec would
// have written it, because that is what reaches an adapter in production: the
// turn machine canonicalizes tool-call arguments, dispatch canonicalizes
// results, and registration canonicalizes schemas. The goldens were captured
// from the generic-tree encoder this request path replaced.
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

// Text, an assistant turn with two nested-object tool calls, one plain and one
// error tool result, a following user message, and two tools with nested
// schemas: every shape the request encoders have to splice.
[[nodiscard]] ModelRequest tool_history_request() {
  return ModelRequest{
      .system_prompt = "Be concise",
      .history =
          std::make_shared<const std::vector<Message>>(
              std::vector<Message>{
                  Message{.role = Role::user,
                          .content = {TextBlock{.text = "Weather in Paris?"}}},
                  Message{.role = Role::assistant,
                          .content =
                              {
                                  TextBlock{.text = "Checking."},
                                  ToolCallBlock{
                                      .id = "call-a",
                                      .name = "weather",
                                      .arguments =
                                          Json{.text = std::string{weather_arguments}},
                                  },
                                  ToolCallBlock{
                                      .id = "call-b",
                                      .name = "almanac",
                                      .arguments =
                                          Json{.text = std::string{almanac_arguments}},
                                  },
                              }},
                  Message{.role = Role::user,
                          .content =
                              {
                                  ToolResultBlock{
                                      .tool_call_id = "call-a",
                                      .result =
                                          Json{.text = std::string{weather_result}},
                                  },
                                  ToolResultBlock{
                                      .tool_call_id = "call-b",
                                      .result =
                                          Json{.text = std::string{almanac_result}},
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
                  .input_schema = Json{.text = std::string{weather_schema}},
              },
              ToolDefinition{
                  .name = "almanac",
                  .description = "Get almanac",
                  .input_schema = Json{.text = std::string{almanac_schema}},
              },
          }),
      .sampling =
          SamplingConfig{
              .temperature = 0.25,
              .top_p = 0.9,
              .max_tokens = 64,
          },
  };
}

// Deliberately not the codec's key order, so a body that still carries these
// bytes proves the encoder spliced the stored text instead of rebuilding it.
constexpr auto unsorted_arguments =
    std::string_view{R"({"zebra":1,"alpha":{"nested":true}})"};
constexpr auto unsorted_schema =
    std::string_view{R"({"type":"object","properties":{"zebra":{"type":"string"}}})"};

[[nodiscard]] ModelRequest unsorted_payload_request() {
  return ModelRequest{
      .messages =
          {
              Message{.role = Role::assistant,
                      .content =
                          {
                              ToolCallBlock{
                                  .id = "call-a",
                                  .name = "lookup",
                                  .arguments =
                                      Json{.text = std::string{unsorted_arguments}},
                              },
                          }},
          },
      .tools = std::make_shared<const std::vector<ToolDefinition>>(
          std::vector<ToolDefinition>{
              ToolDefinition{
                  .name = "lookup",
                  .description = "Look a value up",
                  .input_schema = Json{.text = std::string{unsorted_schema}},
              },
          }),
      .sampling = SamplingConfig{.max_tokens = 64},
  };
}

[[nodiscard]] Config anthropic_config() {
  return Config{
      .base_url = "https://api.anthropic.test/",
      .api_key = "sanitized-test-key",
      .model = "claude-test",
      .dialect = ProviderDialect::anthropic,
  };
}

[[nodiscard]] Config openai_config() {
  return Config{
      .base_url = "https://api.openai.test/v1",
      .api_key = "sanitized-key",
      .model = "chat-model",
      .dialect = ProviderDialect::openai_compatible,
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

// The tool-result document and the OpenAI tool arguments travel as JSON
// strings, so their canonical bytes appear escaped rather than spliced.
[[nodiscard]] std::string as_json_string(const std::string_view text) {
  std::string quoted{};
  for (const char character : text) {
    if (character == '"') {
      quoted.push_back('\\');
    }
    quoted.push_back(character);
  }
  return quoted;
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

TEST_CASE("Anthropic request splices stored JSON text byte for byte") {
  const auto body = encoded_body(anthropic_config(), unsorted_payload_request());
  // Still one JSON document, and still carrying the caller's own spelling.
  REQUIRE(parse_json(body, ErrorCategory::protocol, "body is not valid JSON"));
  CHECK(body.find(std::string{R"("input":)"} + std::string{unsorted_arguments}) !=
        std::string::npos);
  CHECK(body.find(std::string{R"("input_schema":)"} + std::string{unsorted_schema}) !=
        std::string::npos);
}

TEST_CASE("OpenAI request splices stored JSON text byte for byte") {
  const auto body = encoded_body(openai_config(), unsorted_payload_request());
  REQUIRE(parse_json(body, ErrorCategory::protocol, "body is not valid JSON"));
  CHECK(body.find(std::string{R"("parameters":)"} + std::string{unsorted_schema}) !=
        std::string::npos);
  // `arguments` is a JSON string, so the same bytes appear quoted and escaped.
  CHECK(body.find(std::string{R"("arguments":")"} + as_json_string(unsorted_arguments) +
                  '"') != std::string::npos);
}

TEST_CASE("Tool results keep their stored text as the wire string") {
  const auto anthropic = encoded_body(anthropic_config(), tool_history_request());
  const auto openai = encoded_body(openai_config(), tool_history_request());
  const auto quoted = '"' + as_json_string(weather_result) + '"';
  CHECK(anthropic.find(std::string{R"("content":)"} + quoted) != std::string::npos);
  CHECK(openai.find(std::string{R"("content":)"} + quoted) != std::string::npos);
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
          .input_schema = Json{.text = std::move(schema)},
      }});
  request.messages = {
      Message{.role = Role::user, .content = {TextBlock{.text = "go"}}},
      Message{
          .role = Role::assistant,
          .content = {ToolCallBlock{.id = "call-a",
                                    .name = "lookup",
                                    .arguments = Json{.text = std::move(arguments)}}}},
      Message{.role = Role::user,
              .content = {ToolResultBlock{.tool_call_id = "call-a",
                                          .result = Json{.text = std::move(result)}}}},
  };
  return request;
}

[[nodiscard]] bool encodes(const Config& config, const ModelRequest& request) {
  const auto adapter = make_provider_adapter(config.dialect);
  REQUIRE(adapter);
  const auto encoded = adapter->make_request(config, request);
  if (!encoded) {
    CHECK(encoded.error().category == ErrorCategory::invalid_config);
    return false;
  }
  // Whatever was spliced, the body itself must still be one JSON document.
  CHECK(parse_json(encoded->body, ErrorCategory::protocol, "body is not valid JSON"));
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
