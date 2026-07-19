#include "core/model.hpp"
#include "provider/openai.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] TransportResult success_transport() {
  return TransportResult{
      .status_code = 200,
      .provider_request_id = "req-header",
  };
}

void require_protocol(const Result<ModelResponse>& response) {
  REQUIRE_FALSE(response);
  CHECK(response.error().category == ErrorCategory::protocol);
}

} // namespace

TEST_CASE("OpenAI non-streaming text maps usage finish and request correlation") {
  OpenAiAdapter adapter;
  const auto response = adapter.parse_response(
      success_transport(),
      R"({"id":"chatcmpl-not-a-request-id","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"Hello","refusal":null},"finish_reason":"stop"}],"usage":{"prompt_tokens":11,"completion_tokens":5,"total_tokens":16}})");
  REQUIRE(response);
  REQUIRE(response->content.size() == 1);
  CHECK(std::get<TextBlock>(response->content.front()).text == "Hello");
  CHECK(response->finish_reason == FinishReason::completed);
  CHECK(response->usage.input_tokens == 11);
  CHECK(response->usage.output_tokens == 5);
  CHECK(response->provider_request_id == "req-header");
  CHECK(response->provider_request_id != "chatcmpl-not-a-request-id");
}

TEST_CASE("OpenAI non-streaming response preserves multiple function calls") {
  OpenAiAdapter adapter;
  const auto response = adapter.parse_response(
      success_transport(),
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call-a","type":"function","function":{"name":"weather","arguments":"{ \"city\": \"Paris\" }"}},{"id":"call-b","type":"function","function":{"name":"days","arguments":""}}]},"finish_reason":"tool_calls"}],"usage":null})");
  REQUIRE(response);
  REQUIRE(response->content.size() == 2);
  const auto& first = std::get<ToolCallBlock>(response->content[0]);
  const auto& second = std::get<ToolCallBlock>(response->content[1]);
  CHECK(first.id == "call-a");
  CHECK(first.name == "weather");
  CHECK(first.arguments.text == R"({"city":"Paris"})");
  CHECK(second.id == "call-b");
  CHECK(second.name == "days");
  CHECK(second.arguments.text == "{}");
  CHECK(response->finish_reason == FinishReason::tool_use);
}

TEST_CASE("OpenAI finish reasons map without inventing new public values") {
  OpenAiAdapter adapter;
  constexpr std::array cases{
      std::pair{"stop", FinishReason::completed},
      std::pair{"length", FinishReason::length},
      std::pair{"content_filter", FinishReason::unknown},
      std::pair{"future_reason", FinishReason::unknown},
  };
  for (const auto& [wire, expected] : cases) {
    const auto body =
        std::string{
            R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":""},"finish_reason":")"} +
        wire + R"("}]})";
    const auto response = adapter.parse_response(success_transport(), body);
    REQUIRE(response);
    CHECK(response->finish_reason == expected);
  }

  require_protocol(adapter.parse_response(
      success_transport(),
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"function_call":{"name":"legacy","arguments":"{}"}},"finish_reason":"function_call"}]})"));
}

TEST_CASE("OpenAI response rejects malformed required choice and message content") {
  OpenAiAdapter adapter;
  constexpr std::array invalid{
      R"({})",
      R"({"object":7,"choices":[]})",
      R"({"object":"future","choices":[]})",
      R"({"object":"chat.completion"})",
      R"({"object":"chat.completion","choices":{}})",
      R"({"object":"chat.completion","choices":[]})",
      R"({"object":"chat.completion","choices":[{},{}]})",
      R"({"object":"chat.completion","choices":[7]})",
      R"({"object":"chat.completion","choices":[{"message":{},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":"0","message":{},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":1,"message":{"role":"assistant","content":""},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":[],"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"content":""},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"user","content":""},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant"},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":[]},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"","refusal":7},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"","refusal":"blocked"},"finish_reason":"stop"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":{}},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"type":"function","function":{"name":"lookup","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","function":{"name":"lookup","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"function"}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"function","function":[]}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"function","function":{"arguments":"{}"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"function","function":{"name":"lookup"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"function","function":{"name":"","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"function","function":{"name":"lookup","arguments":"{"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"","type":"function","function":{"name":"lookup","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"future","function":{"name":"lookup","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call","type":"function","function":{"name":"lookup","arguments":"[]"}}]},"finish_reason":"tool_calls"}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":""},"finish_reason":null}]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":""},"finish_reason":"stop"}],"usage":[]})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":""},"finish_reason":"stop"}],"usage":{"prompt_tokens":"1"}})",
      R"({"object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":""},"finish_reason":"stop"}],"usage":{"completion_tokens":"1"}})",
  };
  for (const auto body : invalid) {
    INFO(body);
    require_protocol(adapter.parse_response(success_transport(), body));
  }
}

TEST_CASE("OpenAI in-band error aliases map to stable public categories") {
  OpenAiAdapter adapter;
  struct Case {
    std::string_view token;
    ErrorCategory category;
  };
  constexpr std::array cases{
      Case{"authentication_error", ErrorCategory::authentication},
      Case{"permission_error", ErrorCategory::authentication},
      Case{"invalid_api_key", ErrorCategory::authentication},
      Case{"rate_limit_error", ErrorCategory::rate_limit},
      Case{"rate_limit_exceeded", ErrorCategory::rate_limit},
      Case{"insufficient_quota", ErrorCategory::rate_limit},
      Case{"server_error", ErrorCategory::network},
      Case{"api_error", ErrorCategory::network},
      Case{"overloaded_error", ErrorCategory::network},
      Case{"future_error", ErrorCategory::protocol},
  };
  for (const auto& [token, category] : cases) {
    const auto body =
        std::string{R"({"error":{"type":")"} + std::string{token} + R"("}})";
    const auto response = adapter.parse_response(success_transport(), body);
    REQUIRE_FALSE(response);
    CHECK(response.error().category == category);
    CHECK(response.error().provider_detail == "openai:" + std::string{token});
    CHECK(response.error().retryable == (category == ErrorCategory::rate_limit ||
                                         category == ErrorCategory::network));
  }
}

TEST_CASE("OpenAI error classification checks type then code for recognized aliases") {
  OpenAiAdapter adapter;
  struct Case {
    std::string_view body;
    std::string_view detail;
    ErrorCategory category;
  };
  constexpr std::array cases{
      Case{R"({"error":{"type":"invalid_request_error","code":"invalid_api_key"}})",
           "invalid_api_key", ErrorCategory::authentication},
      Case{R"({"error":{"type":"future_error","code":"rate_limit_exceeded"}})",
           "rate_limit_exceeded", ErrorCategory::rate_limit},
      Case{R"({"error":{"type":"authentication_error","code":"rate_limit_exceeded"}})",
           "authentication_error", ErrorCategory::authentication},
  };
  for (const auto& [body, detail, category] : cases) {
    const auto response = adapter.parse_response(success_transport(), body);
    REQUIRE_FALSE(response);
    CHECK(response.error().category == category);
    CHECK(response.error().provider_detail == "openai:" + std::string{detail});
    CHECK(response.error().retryable == (category == ErrorCategory::rate_limit ||
                                         category == ErrorCategory::network));
  }
}

TEST_CASE("OpenAI error token fallbacks are bounded and deterministic") {
  OpenAiAdapter adapter;
  constexpr std::array cases{
      std::pair{R"({"error":null})", "unknown_error"},
      std::pair{R"({"error":[]})", "unknown_error"},
      std::pair{R"({"error":{}})", "unknown_error"},
      std::pair{R"({"error":{"type":7}})", "unknown_error"},
      std::pair{R"({"error":{"type":7,"code":"api_error"}})", "api_error"},
      std::pair{R"({"error":{"code":429}})", "429"},
      std::pair{R"({"error":{"type":""}})", "unknown_error"},
  };
  for (const auto& [body, token] : cases) {
    const auto response = adapter.parse_response(success_transport(), body);
    REQUIRE_FALSE(response);
    CHECK(response.error().provider_detail == "openai:" + std::string{token});
  }

  const auto oversized =
      std::string{R"({"error":{"type":")"} + std::string(97, 'a') + R"("}})";
  const auto response = adapter.parse_response(success_transport(), oversized);
  REQUIRE_FALSE(response);
  CHECK(response.error().provider_detail == "openai:unknown_error");
}

TEST_CASE("OpenAI HTTP and in-band errors remain categorized and secret safe") {
  OpenAiAdapter adapter;
  constexpr auto body =
      R"({"error":{"type":"rate_limit_error","code":"unsafe-secret-value","message":"private-token"}})";
  constexpr std::array cases{
      std::pair{401, ErrorCategory::authentication},
      std::pair{403, ErrorCategory::authentication},
      std::pair{429, ErrorCategory::rate_limit},
      std::pair{500, ErrorCategory::network},
      std::pair{400, ErrorCategory::protocol},
  };
  for (const auto& [status, category] : cases) {
    const auto response = adapter.parse_response(
        TransportResult{
            .status_code = status,
            .provider_request_id = "req-error",
        },
        body);
    REQUIRE_FALSE(response);
    CHECK(response.error().category == category);
    CHECK(response.error().provider_request_id == "req-error");
    CHECK(response.error().provider_detail == "openai:rate_limit_error");
    CHECK(response.error().message.find("private-token") == std::string::npos);
    CHECK(response.error().retryable == (category == ErrorCategory::rate_limit ||
                                         category == ErrorCategory::network));
  }

  const auto in_band = adapter.parse_response(
      success_transport(),
      R"({"error":{"type":"overloaded_error","message":"private-token"}})");
  REQUIRE_FALSE(in_band);
  CHECK(in_band.error().category == ErrorCategory::network);
  CHECK(in_band.error().retryable);
  CHECK(in_band.error().provider_detail == "openai:overloaded_error");

  const auto unsafe = adapter.parse_response(
      success_transport(),
      R"({"error":{"type":"unsafe-secret-value","message":"private-token"}})");
  REQUIRE_FALSE(unsafe);
  CHECK(unsafe.error().category == ErrorCategory::protocol);
  CHECK(unsafe.error().provider_detail == "openai:unknown_error");
  CHECK(unsafe.error().provider_detail.find("secret") == std::string::npos);

  for (const auto status : {199, 600}) {
    const auto malformed =
        adapter.parse_response(TransportResult{.status_code = status}, "{");
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().category == ErrorCategory::protocol);
    CHECK(malformed.error().provider_detail == "openai:unknown_error");
  }
}
