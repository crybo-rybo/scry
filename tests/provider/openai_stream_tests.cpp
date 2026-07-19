#include "core/model.hpp"
#include "provider/openai.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace scry;
using namespace scry::detail;

[[nodiscard]] Result<std::vector<ProviderEvent>>
event(OpenAiAdapter& adapter, ProviderDecodeState& state, const std::string_view data,
      const std::string_view name = "message") {
  return adapter.parse_stream_event(name, data, state);
}

void require_protocol(const Result<std::vector<ProviderEvent>>& result) {
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::protocol);
}

void apply(OpenAiAdapter& adapter, ProviderDecodeState& state,
           const std::string_view data) {
  REQUIRE(event(adapter, state, data));
}

[[nodiscard]] std::string chunk(const std::string_view choice,
                                const std::string_view id = "chatcmpl-stream") {
  return std::string{R"({"id":")"} + std::string{id} +
         R"(","object":"chat.completion.chunk","choices":[)" + std::string{choice} +
         "]}";
}

[[nodiscard]] std::string tool_chunk(const std::string_view fragment) {
  return chunk(std::string{R"({"index":0,"delta":{"tool_calls":[)"} +
               std::string{fragment} + R"(]},"finish_reason":null})");
}

} // namespace

TEST_CASE("OpenAI stream completes only after post-finish usage and DONE") {
  OpenAiAdapter adapter;
  ProviderDecodeState state{};
  state.response.provider_request_id = "req-header";

  apply(adapter, state,
        chunk(R"({"index":0,"delta":{"role":"assistant"},"finish_reason":null})"));
  auto first =
      event(adapter, state,
            chunk(R"({"index":0,"delta":{"content":"Hello "},"finish_reason":null})"));
  REQUIRE(first);
  REQUIRE(first->size() == 1);
  CHECK(std::get<ProviderTextDelta>(first->front()).text == "Hello ");
  auto second =
      event(adapter, state,
            chunk(R"({"index":0,"delta":{"content":"world"},"finish_reason":null})"));
  REQUIRE(second);
  CHECK(std::get<ProviderTextDelta>(second->front()).text == "world");

  apply(adapter, state, chunk(R"({"index":0,"delta":{},"finish_reason":"stop"})"));
  CHECK_FALSE(state.completed);
  apply(
      adapter, state,
      R"({"id":"chatcmpl-stream","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15}})");
  const auto completed = event(adapter, state, "[DONE]");
  REQUIRE(completed);
  REQUIRE(completed->size() == 1);
  const auto& response = std::get<ProviderCompleted>(completed->front()).response;
  REQUIRE(response.content.size() == 1);
  CHECK(std::get<TextBlock>(response.content.front()).text == "Hello world");
  CHECK(response.finish_reason == FinishReason::completed);
  CHECK(response.usage.input_tokens == 12);
  CHECK(response.usage.output_tokens == 3);
  CHECK(response.provider_request_id == "req-header");
  CHECK(state.semantic_output_consumed);
  CHECK(state.completed);
}

TEST_CASE("OpenAI stream usage objects replace previous totals") {
  OpenAiAdapter adapter;
  ProviderDecodeState state{};
  apply(
      adapter, state,
      R"({"id":"chatcmpl-stream","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}],"usage":{"prompt_tokens":8,"completion_tokens":2}})");
  CHECK(state.response.usage.input_tokens == 8);
  CHECK(state.response.usage.output_tokens == 2);

  apply(
      adapter, state,
      R"({"id":"chatcmpl-stream","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"completion_tokens":5}})");
  CHECK(state.response.usage.input_tokens == 0);
  CHECK(state.response.usage.output_tokens == 5);
  REQUIRE(event(adapter, state, "[DONE]"));
}

TEST_CASE("OpenAI stream preserves interleaved tool fragments in index order") {
  OpenAiAdapter adapter;
  ProviderDecodeState state{};
  apply(
      adapter, state,
      chunk(
          R"({"index":0,"delta":{"tool_calls":[{"index":1,"id":"call-b","type":"function","function":{"name":"days","arguments":"{\"days\":"}}]},"finish_reason":null})"));
  apply(
      adapter, state,
      chunk(
          R"({"index":0,"delta":{"tool_calls":[{"index":0,"id":"call-a","type":"function","function":{"name":"weather","arguments":"{\"city\":\""}}]},"finish_reason":null})"));
  apply(
      adapter, state,
      chunk(
          R"({"index":0,"delta":{"tool_calls":[{"index":1,"function":{"arguments":"3}"}}]},"finish_reason":null})"));
  apply(
      adapter, state,
      chunk(
          R"({"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"Paris\"}"}}]},"finish_reason":null})"));
  apply(adapter, state,
        chunk(R"({"index":0,"delta":{},"finish_reason":"tool_calls"})"));
  const auto completed = event(adapter, state, "[DONE]");
  REQUIRE(completed);
  const auto& response = std::get<ProviderCompleted>(completed->front()).response;
  REQUIRE(response.content.size() == 2);
  const auto& first = std::get<ToolCallBlock>(response.content[0]);
  const auto& second = std::get<ToolCallBlock>(response.content[1]);
  CHECK(first.id == "call-a");
  CHECK(first.name == "weather");
  CHECK(first.arguments.text == R"({"city":"Paris"})");
  CHECK(second.id == "call-b");
  CHECK(second.name == "days");
  CHECK(second.arguments.text == R"({"days":3})");
  CHECK(response.finish_reason == FinishReason::tool_use);
  CHECK(state.semantic_output_consumed);
}

TEST_CASE("OpenAI stream accepts repeated metadata and normalizes empty arguments") {
  OpenAiAdapter adapter;
  ProviderDecodeState state{};
  const auto metadata =
      R"({"index":0,"id":"call","type":"function","function":{"name":"ping","arguments":""}})";
  apply(adapter, state,
        chunk(std::string{R"({"index":0,"delta":{"tool_calls":[)"} + metadata +
              R"(]},"finish_reason":null})"));
  apply(adapter, state,
        chunk(std::string{R"({"index":0,"delta":{"tool_calls":[)"} + metadata +
              R"(]},"finish_reason":null})"));
  apply(adapter, state,
        chunk(R"({"index":0,"delta":{},"finish_reason":"tool_calls"})"));

  const auto completed = event(adapter, state, "[DONE]");
  REQUIRE(completed);
  const auto& response = std::get<ProviderCompleted>(completed->front()).response;
  REQUIRE(response.content.size() == 1);
  const auto& call = std::get<ToolCallBlock>(response.content.front());
  CHECK(call.id == "call");
  CHECK(call.name == "ping");
  CHECK(call.arguments.text == "{}");
}

TEST_CASE("OpenAI streamed argument limit rejects before appending") {
  OpenAiAdapter adapter;
  ProviderDecodeState state{.max_tool_arguments_bytes = 7};
  apply(
      adapter, state,
      chunk(
          R"({"index":0,"delta":{"tool_calls":[{"index":0,"id":"call","type":"function","function":{"name":"lookup","arguments":"{\"x\":1}"}}]},"finish_reason":null})"));
  const auto over = event(
      adapter, state,
      chunk(
          R"({"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":" "}}]},"finish_reason":null})"));
  REQUIRE_FALSE(over);
  CHECK(over.error().category == ErrorCategory::resource_limit);
  const auto& decode = std::get<OpenAiProviderDecodeState>(state.dialect);
  CHECK(decode.tool_calls.at(0).arguments == R"({"x":1})");
}

TEST_CASE("OpenAI stream rejects incomplete and conflicting tool metadata") {
  OpenAiAdapter adapter;

  SECTION("sparse indices") {
    ProviderDecodeState state{};
    apply(
        adapter, state,
        chunk(
            R"({"index":0,"delta":{"tool_calls":[{"index":1,"id":"call","type":"function","function":{"name":"lookup","arguments":"{}"}}]},"finish_reason":null})"));
    require_protocol(
        event(adapter, state,
              chunk(R"({"index":0,"delta":{},"finish_reason":"tool_calls"})")));
  }

  SECTION("metadata changes") {
    ProviderDecodeState state{};
    apply(
        adapter, state,
        chunk(
            R"({"index":0,"delta":{"tool_calls":[{"index":0,"id":"first","type":"function","function":{"name":"lookup","arguments":""}}]},"finish_reason":null})"));
    require_protocol(event(
        adapter, state,
        chunk(
            R"({"index":0,"delta":{"tool_calls":[{"index":0,"id":"second"}]},"finish_reason":null})")));
  }

  SECTION("arguments are not an object") {
    ProviderDecodeState state{};
    apply(
        adapter, state,
        chunk(
            R"({"index":0,"delta":{"tool_calls":[{"index":0,"id":"call","type":"function","function":{"name":"lookup","arguments":"[]"}}]},"finish_reason":null})"));
    require_protocol(
        event(adapter, state,
              chunk(R"({"index":0,"delta":{},"finish_reason":"tool_calls"})")));
  }
}

TEST_CASE("OpenAI stream rejects malformed tool fragments at each boundary") {
  OpenAiAdapter adapter;
  constexpr std::string_view invalid[]{
      "7",
      R"({})",
      R"({"index":"0"})",
      R"({"index":0,"id":""})",
      R"({"index":0,"type":7})",
      R"({"index":0,"type":"future"})",
      R"({"index":0,"function":[]})",
      R"({"index":0,"function":{"name":""}})",
      R"({"index":0,"function":{"name":7}})",
      R"({"index":0,"function":{"arguments":7}})",
  };
  for (const auto fragment : invalid) {
    INFO(fragment);
    ProviderDecodeState state{};
    require_protocol(event(adapter, state, tool_chunk(fragment)));
  }

  ProviderDecodeState nullable{};
  apply(adapter, nullable, tool_chunk(R"({"index":0,"function":null})"));
  apply(adapter, nullable, tool_chunk(R"({"index":0,"function":{"arguments":null}})"));

  ProviderDecodeState state{};
  apply(
      adapter, state,
      tool_chunk(
          R"({"index":0,"id":"call","type":"function","function":{"name":"first"}})"));
  require_protocol(event(
      adapter, state,
      tool_chunk(R"({"index":0,"function":{"name":"second","arguments":null}})")));
}

TEST_CASE("OpenAI stream enforces finish and terminal lifecycle") {
  OpenAiAdapter adapter;

  SECTION("optional events before finish are ignored") {
    ProviderDecodeState state{};
    const auto ignored = event(adapter, state, "opaque", "future_optional");
    REQUIRE(ignored);
    REQUIRE(ignored->size() == 1);
    CHECK(std::get<ProviderIgnoredEvent>(ignored->front()).name == "future_optional");
    apply(
        adapter, state,
        chunk(R"({"index":0,"delta":{"function_call":null},"finish_reason":"stop"})"));
    REQUIRE(event(adapter, state, "[DONE]"));
  }

  SECTION("DONE requires finish") {
    ProviderDecodeState state{};
    require_protocol(event(adapter, state, "[DONE]"));
  }

  SECTION("chunk IDs remain stable") {
    ProviderDecodeState state{};
    apply(adapter, state,
          chunk(R"({"index":0,"delta":{},"finish_reason":null})", "first"));
    require_protocol(
        event(adapter, state,
              chunk(R"({"index":0,"delta":{},"finish_reason":"stop"})", "second")));
  }

  SECTION("semantic and optional events after finish fail") {
    ProviderDecodeState state{};
    apply(adapter, state, chunk(R"({"index":0,"delta":{},"finish_reason":"stop"})"));
    require_protocol(
        event(adapter, state,
              chunk(R"({"index":0,"delta":{"content":"late"},"finish_reason":null})")));
    require_protocol(event(adapter, state, "ignored", "future_optional"));
  }

  SECTION("duplicate terminal marker fails") {
    ProviderDecodeState state{};
    apply(adapter, state, chunk(R"({"index":0,"delta":{},"finish_reason":"stop"})"));
    REQUIRE(event(adapter, state, "[DONE]"));
    require_protocol(event(adapter, state, "[DONE]"));
  }
}

TEST_CASE("OpenAI stream rejects malformed required chunk shapes") {
  OpenAiAdapter adapter;
  constexpr std::string_view invalid[]{
      "{broken",
      R"({"id":"id","object":"future","choices":[]})",
      R"({"id":"","object":"chat.completion.chunk","choices":[]})",
      R"({"id":"id","object":"chat.completion.chunk","choices":{}})",
      R"({"id":"id","object":"chat.completion.chunk","choices":[{},{}]})",
      R"({"id":"id","object":"chat.completion.chunk","choices":[{"index":1,"delta":{},"finish_reason":null}]})",
      R"({"id":"id","object":"chat.completion.chunk","choices":[{"index":0,"finish_reason":null}]})",
      R"({"id":"id","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"user"},"finish_reason":null}]})",
      R"({"id":"id","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":[]},"finish_reason":null}]})",
      R"({"id":"id","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"function_call":{}},"finish_reason":null}]})",
      R"({"id":"id","object":"chat.completion.chunk","choices":[],"usage":null})",
  };
  for (const auto body : invalid) {
    INFO(body);
    ProviderDecodeState state{};
    require_protocol(event(adapter, state, body));
  }
}

TEST_CASE("OpenAI stream maps errors and isolates dialect state") {
  OpenAiAdapter adapter;
  ProviderDecodeState state{};
  const auto error =
      event(adapter, state,
            R"({"error":{"type":"overloaded_error","message":"private"}})", "error");
  REQUIRE_FALSE(error);
  CHECK(error.error().category == ErrorCategory::network);
  CHECK(error.error().retryable);
  CHECK(error.error().provider_detail == "openai:overloaded_error");
  CHECK(error.error().message.find("private") == std::string::npos);

  ProviderDecodeState mislabeled{};
  mislabeled.response.provider_request_id = "req-mislabeled";
  const auto root_error =
      event(adapter, mislabeled,
            R"({"error":{"type":"invalid_api_key","message":"private"}})", "telemetry");
  REQUIRE_FALSE(root_error);
  CHECK(root_error.error().category == ErrorCategory::authentication);
  CHECK(root_error.error().provider_detail == "openai:invalid_api_key");
  CHECK(root_error.error().provider_request_id == "req-mislabeled");
  CHECK(root_error.error().message.find("private") == std::string::npos);

  ProviderDecodeState mixed_error{};
  const auto code_alias = event(
      adapter, mixed_error,
      R"({"error":{"type":"future_error","code":"rate_limit_exceeded"}})", "error");
  REQUIRE_FALSE(code_alias);
  CHECK(code_alias.error().category == ErrorCategory::rate_limit);
  CHECK(code_alias.error().retryable);
  CHECK(code_alias.error().provider_detail == "openai:rate_limit_exceeded");

  ProviderDecodeState named_terminal{};
  require_protocol(event(adapter, named_terminal, "[DONE]", "error"));

  ProviderDecodeState unknown_error{};
  const auto fallback =
      event(adapter, unknown_error, R"({"message":"private"})", "error");
  REQUIRE_FALSE(fallback);
  CHECK(fallback.error().provider_detail == "openai:unknown_error");

  ProviderDecodeState foreign{};
  foreign.dialect.emplace<AnthropicProviderDecodeState>();
  require_protocol(
      event(adapter, foreign, chunk(R"({"index":0,"delta":{},"finish_reason":null})")));
}
