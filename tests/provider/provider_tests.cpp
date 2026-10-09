#include "core/model.hpp"
#include "core/provider.hpp"
#include "fixture_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
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

} // namespace

TEST_CASE("Anthropic request merges consecutive same-role messages") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);

  const auto encoded =
      adapter->make_request(anthropic_config(), tool_result_history_request());
  REQUIRE(encoded);
  const auto body = json_view(encoded->body);
  // user, assistant, then the tool results and the next question as one message.
  CHECK(member_strings(body, "messages", "role") ==
        std::vector<std::string>{"user", "assistant", "user"});

  const auto merged = body.find("messages")->at(2);
  REQUIRE(merged);
  CHECK(member_strings(*merged, "content", "type") ==
        std::vector<std::string>{"tool_result", "text"});
}

namespace {

// The history a typed turn commits after it removed a rejected answer: the prose
// of that round stays as an assistant message of its own, so the assistant
// message of the next round follows it directly. The accepted answer is prose
// followed by the answer JSON as a second text block.
[[nodiscard]] ModelRequest adjacent_assistant_history_request() {
  auto model_request = request();
  model_request.history =
      std::make_shared<const std::vector<Message>>(
          std::vector<Message>{
              Message{.role = Role::user, .content = {TextBlock{.text = "question"}}},
              Message{.role = Role::assistant,
                      .content = {TextBlock{.text = "Let me answer."}}},
              Message{.role = Role::assistant,
                      .content =
                          {
                              TextBlock{.text = "Looking it up."},
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
              Message{.role = Role::assistant,
                      .content =
                          {
                              TextBlock{.text = "Found it."},
                              TextBlock{.text = R"({"answer":42})"},
                          }},
          });
  model_request.messages = {
      Message{.role = Role::user, .content = {TextBlock{.text = "Thanks."}}},
  };
  return model_request;
}

// The string member `field` of the element at `index` of the array at `name`.
[[nodiscard]] std::string element_string(const JsonView& owner,
                                         const std::string_view name,
                                         const std::size_t index,
                                         const std::string_view field) {
  const auto array = owner.find(name);
  REQUIRE(array);
  const auto element = array->at(index);
  REQUIRE(element);
  const auto value = element->find(field);
  REQUIRE(value);
  REQUIRE(value->string());
  return std::string{*value->string()};
}

} // namespace

TEST_CASE("Anthropic request merges adjacent assistant messages in order") {
  const auto adapter = make_provider_adapter(ProviderDialect::anthropic);
  REQUIRE(adapter);

  const auto encoded =
      adapter->make_request(anthropic_config(), adjacent_assistant_history_request());
  REQUIRE(encoded);
  const auto body = json_view(encoded->body);
  CHECK(member_strings(body, "messages", "role") ==
        std::vector<std::string>{"user", "assistant", "user", "assistant", "user"});

  const auto merged = body.find("messages")->at(1);
  REQUIRE(merged);
  CHECK(member_strings(*merged, "content", "type") ==
        std::vector<std::string>{"text", "text", "tool_use"});
  CHECK(element_string(*merged, "content", 0, "text") == "Let me answer.");
  CHECK(element_string(*merged, "content", 1, "text") == "Looking it up.");
  CHECK(element_string(*merged, "content", 2, "id") == "tool_1");
}

TEST_CASE("OpenAI request keeps adjacent assistant messages and joins their text") {
  const auto adapter = make_provider_adapter(ProviderDialect::openai_compatible);
  REQUIRE(adapter);

  const auto encoded =
      adapter->make_request(openai_config(), adjacent_assistant_history_request());
  REQUIRE(encoded);
  const auto body = json_view(encoded->body);
  CHECK(member_strings(body, "messages", "role") ==
        std::vector<std::string>{"system", "user", "assistant", "assistant", "tool",
                                 "assistant", "user"});
  CHECK(element_string(body, "messages", 2, "content") == "Let me answer.");
  CHECK(element_string(body, "messages", 3, "content") == "Looking it up.");
  // The text blocks of one message are joined without a separator.
  CHECK(element_string(body, "messages", 5, "content") == R"(Found it.{"answer":42})");
}
