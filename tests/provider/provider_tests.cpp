#include "core/json_codec.hpp"
#include "core/model.hpp"
#include "core/provider.hpp"
#include "fixture_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>

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

} // namespace

TEST_CASE("provider factory exposes both supported dialects") {
  CHECK(make_provider_adapter(ProviderDialect::anthropic) != nullptr);
  CHECK(make_provider_adapter(ProviderDialect::openai_compatible) != nullptr);
}

TEST_CASE("Anthropic request is semantically equivalent to its sanitized fixture") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);

  const auto encoded = adapter->make_request(anthropic_config(), request());
  REQUIRE(encoded.has_value());
  CHECK(encoded->url == "https://api.anthropic.test/v1/messages");
  CHECK(encoded->tls_verify_peer);
  // The transport prefixes body-derived error tokens with this namespace, so it
  // must match the one the stream decoder uses.
  CHECK(encoded->provider_namespace == "anthropic");
  CHECK(header(*encoded, "content-type") == "application/json");
  CHECK(header(*encoded, "x-api-key") == "sanitized-test-key");
  CHECK(header(*encoded, "anthropic-version") == "2023-06-01");
  CHECK(header(*encoded, "accept") == "text/event-stream");
  CHECK(canonical(encoded->body) ==
        canonical(scry::test_fixtures::anthropic_fixture("request.json")));
}

TEST_CASE("Anthropic request carries the configured network options") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);

  auto networked = anthropic_config();
  networked.extra_headers = {HttpHeader{.name = "x-scry-example", .value = "1"}};
  networked.ca_bundle_path = "/tmp/ca.pem";
  networked.proxy = "http://proxy.internal:3128";

  const auto encoded = adapter->make_request(networked, request());
  REQUIRE(encoded.has_value());
  CHECK(encoded->ca_bundle_path == "/tmp/ca.pem");
  CHECK(encoded->proxy == "http://proxy.internal:3128");
  REQUIRE(encoded->headers.size() == 5);
  CHECK(encoded->headers.back().name == "x-scry-example");
  CHECK(encoded->headers.back().value == "1");
  CHECK(header(*encoded, "x-api-key") == "sanitized-test-key");
}

namespace {

// The history a turn leaves behind when it stopped at the tool-round limit: the
// last committed message is the user message carrying that round's results, and
// the next send appends its own user message straight after it.
[[nodiscard]] ModelRequest tool_result_history_request() {
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
  return model_request;
}

[[nodiscard]] std::string block_type(const JsonValue& block) {
  const auto type = required_json_string(block, "type");
  REQUIRE(type);
  return std::string{*type};
}

} // namespace

TEST_CASE("Anthropic request merges consecutive same-role messages") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);

  const auto encoded =
      adapter->make_request(anthropic_config(), tool_result_history_request());
  REQUIRE(encoded);
  const auto body =
      parse_json(encoded->body, ErrorCategory::protocol, "body is not valid JSON");
  REQUIRE(body);
  const auto messages = required_json_array(*body, "messages");
  REQUIRE(messages);
  // user, assistant, then the tool results and the next question as one message.
  REQUIRE((*messages)->size() == 3);

  const auto& merged = (**messages)[2];
  const auto role = required_json_string(merged, "role");
  REQUIRE(role);
  CHECK(*role == "user");
  const auto content = required_json_array(merged, "content");
  REQUIRE(content);
  REQUIRE((*content)->size() == 2);
  CHECK(block_type((**content)[0]) == "tool_result");
  CHECK(block_type((**content)[1]) == "text");
}
