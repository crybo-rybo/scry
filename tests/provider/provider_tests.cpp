#include "core/model.hpp"
#include "core/provider.hpp"
#include "provider/wire_json.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

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
      .streaming = false,
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

TEST_CASE("provider factory exposes only the M1 Anthropic dialect") {
  auto anthropic = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(anthropic.has_value());
  CHECK(*anthropic != nullptr);

  auto deferred = make_provider_adapter(ProviderDialect::openai_compatible);
  REQUIRE_FALSE(deferred.has_value());
  CHECK(deferred.error().category == ErrorCategory::invalid_config);
  CHECK(deferred.error().message.find("M4") != std::string::npos);
}

TEST_CASE("Anthropic request encoding matches the sanitized golden fixture") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());

  const auto encoded = (*adapter)->make_request(config(), request());
  REQUIRE(encoded.has_value());
  CHECK(encoded->url == "https://api.anthropic.test/v1/messages");
  CHECK(encoded->streaming == false);
  CHECK(encoded->tls_verify_peer);
  CHECK(header(*encoded, "content-type") == "application/json");
  CHECK(header(*encoded, "x-api-key") == "sanitized-test-key");
  CHECK(header(*encoded, "anthropic-version") == "2023-06-01");
  CHECK(header(*encoded, "accept") == "application/json");
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

TEST_CASE("Anthropic non-streaming response preserves content and correlation") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  const TransportResult transport{
      .status_code = 200,
      .provider_request_id = "req_header_sanitized",
  };

  const auto response = (*adapter)->parse_response(transport, fixture("response.json"));
  REQUIRE(response.has_value());
  REQUIRE(response->content.size() == 1);
  const auto* text = std::get_if<TextBlock>(&response->content.front());
  REQUIRE(text != nullptr);
  CHECK(text->text == "Hello from Anthropic.");
  CHECK(response->finish_reason == FinishReason::completed);
  CHECK(response->usage.input_tokens == 11);
  CHECK(response->usage.output_tokens == 5);
  CHECK(response->provider_request_id == "req_header_sanitized");
}

TEST_CASE("Anthropic response rejects malformed and required unknown content") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  const TransportResult transport{.status_code = 200};

  auto response = (*adapter)->parse_response(transport, "{broken");
  REQUIRE_FALSE(response.has_value());
  CHECK(response.error().category == ErrorCategory::protocol);

  response = (*adapter)->parse_response(
      transport,
      R"({"type":"message","content":[{"type":"future_required"}],"stop_reason":"end_turn"})");
  REQUIRE_FALSE(response.has_value());
  CHECK(response.error().category == ErrorCategory::protocol);
}

TEST_CASE("Anthropic HTTP failures map to stable categories and safe detail") {
  auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter.has_value());
  constexpr auto body =
      R"({"type":"error","error":{"type":"test_error","message":"sk-ant-secret"},"request_id":"req_body"})";
  constexpr std::array cases{
      std::pair{401, ErrorCategory::authentication},
      std::pair{403, ErrorCategory::authentication},
      std::pair{429, ErrorCategory::rate_limit},
      std::pair{500, ErrorCategory::network},
      std::pair{400, ErrorCategory::protocol},
  };

  for (const auto& [status, category] : cases) {
    INFO("HTTP status " << status);
    const TransportResult transport{
        .status_code = static_cast<std::int32_t>(status),
        .provider_request_id = "req_header",
    };
    const auto response = (*adapter)->parse_response(transport, body);
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().category == category);
    CHECK(response.error().provider_request_id == "req_header");
    CHECK(response.error().provider_detail == "anthropic:test_error");
    CHECK(response.error().message.find("sk-ant-secret") == std::string::npos);
    CHECK(response.error().provider_detail.find("sk-ant-secret") == std::string::npos);
    CHECK(response.error().retryable == (status == 429 || status == 500));
  }

  const auto unsafe = (*adapter)->parse_response(
      TransportResult{.status_code = 400},
      R"({"type":"error","error":{"type":"unsafe-secret-value"}})");
  REQUIRE_FALSE(unsafe);
  CHECK(unsafe.error().provider_detail == "anthropic:unknown_error");
  CHECK(unsafe.error().provider_detail.find("secret") == std::string::npos);
}
