#include "core/model.hpp"
#include "core/provider.hpp"
#include "provider/wire_json.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] std::string fixture(const std::string_view name) {
  std::ifstream input{std::string{SCRY_ANTHROPIC_FIXTURE_DIR} + "/" +
                      std::string{name}};
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string canonical(const std::string_view json) {
  auto parsed = parse_wire_json(json, ErrorCategory::protocol, "Fixture is invalid");
  REQUIRE(parsed.has_value());
  auto encoded =
      write_wire_json(*parsed, ErrorCategory::protocol, "Fixture could not be encoded");
  REQUIRE(encoded.has_value());
  return *encoded;
}

[[nodiscard]] Config config() {
  return Config{
      .base_url = "https://api.anthropic.test/",
      .api_key = "sanitized-test-key",
      .model = "fallback-model",
      .dialect = ProviderDialect::anthropic,
  };
}

[[nodiscard]] ModelRequest request() {
  return ModelRequest{
      .model = "claude-test",
      .system_prompt = "Be concise",
      .messages =
          {
              Message{
                  .role = Role::user,
                  .content = {TextBlock{.text = "Hello"}},
              },
          },
      .sampling =
          SamplingConfig{
              .temperature = 0.25,
              .top_p = 0.9,
              .max_tokens = 64,
          },
  };
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

} // namespace

TEST_CASE("provider factory exposes both supported dialects") {
  auto anthropic = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(anthropic.has_value());
  CHECK(*anthropic != nullptr);

  auto openai = make_provider_adapter(ProviderDialect::openai_compatible);
  REQUIRE(openai.has_value());
  CHECK(*openai != nullptr);
}

TEST_CASE("Anthropic request encoding matches the sanitized golden fixture") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());

  const auto encoded = (*adapter)->make_request(config(), request());
  REQUIRE(encoded.has_value());
  CHECK(encoded->url == "https://api.anthropic.test/v1/messages");
  CHECK(encoded->tls_verify_peer);
  CHECK(header(*encoded, "content-type") == "application/json");
  CHECK(header(*encoded, "x-api-key") == "sanitized-test-key");
  CHECK(header(*encoded, "anthropic-version") == "2023-06-01");
  CHECK(header(*encoded, "accept") == "text/event-stream");
  CHECK(canonical(encoded->body) == canonical(fixture("request.json")));
}

TEST_CASE("Anthropic request encoding preserves neutral tool shapes") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  auto model_request = request();
  model_request.tools.push_back(ToolSchema{
      .name = "lookup",
      .description = "Lookup a value",
      .input_schema = Json{.text = R"({"type":"object"})"},
  });
  model_request.messages.push_back(Message{
      .role = Role::assistant,
      .content =
          {
              ToolCallBlock{
                  .id = "tool_1",
                  .name = "lookup",
                  .arguments = Json{.text = R"({"key":"value"})"},
              },
          },
  });
  model_request.messages.push_back(Message{
      .role = Role::user,
      .content =
          {
              ToolResultBlock{
                  .tool_call_id = "tool_1",
                  .result = Json{.text = R"({"answer":42})"},
              },
          },
  });

  const auto encoded = (*adapter)->make_request(config(), model_request);
  REQUIRE(encoded.has_value());
  CHECK(encoded->body.find(R"("type":"tool_use")") != std::string::npos);
  CHECK(encoded->body.find(R"("type":"tool_result")") != std::string::npos);
  CHECK(encoded->body.find(R"("input_schema":{"type":"object"})") != std::string::npos);
}

TEST_CASE("Anthropic request validation reports value errors without secrets") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  auto invalid = request();
  invalid.sampling.max_tokens.reset();

  auto result = (*adapter)->make_request(config(), invalid);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::invalid_config);
  CHECK(result.error().message.find("sanitized-test-key") == std::string::npos);

  invalid = request();
  invalid.messages.front().content = {ToolCallBlock{
      .id = "tool_1",
      .name = "lookup",
      .arguments = Json{.text = "not-json"},
  }};
  result = (*adapter)->make_request(config(), invalid);
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().category == ErrorCategory::invalid_config);
}
