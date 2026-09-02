#include "support/transport/loopback_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace {

constexpr auto successful_stream = std::string_view{R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_curl","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":7,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello from curl."}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":4}}

event: message_stop
data: {"type":"message_stop"}

)"};

constexpr auto openai_successful_stream = std::string_view{
    R"(data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}

data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Hello from OpenAI-compatible curl."},"finish_reason":null}]}

data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":8,"completion_tokens":5,"total_tokens":13}}

data: [DONE]

)"};

[[nodiscard]] std::string response(const std::string_view status,
                                   const std::string_view headers,
                                   const std::string_view body) {
  return "HTTP/1.1 " + std::string{status} + "\r\n" + std::string{headers} +
         "Content-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + std::string{body};
}

[[nodiscard]] scry::Config config_for(const scry::test::LoopbackServer& server) {
  scry::Config config{
      .base_url = server.url(),
      .api_key = "curl-integration-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.timeouts.connect = 500ms;
  // No total bound: held transfers are ended by cancellation or destruction.
  config.timeouts.idle = 2s;
  config.timeouts.transfer = std::nullopt;
  config.timeouts.shutdown = 25ms;
  return config;
}

[[nodiscard]] scry::Config openai_config_for(const scry::test::LoopbackServer& server) {
  auto config = config_for(server);
  config.base_url = server.url("/v1/");
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

[[nodiscard]] std::string ascii_lower(std::string value) {
  for (auto& character : value) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate,
                              const std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(harness.update());
    if (std::forward<Predicate>(predicate)()) {
      return true;
    }
    std::this_thread::yield();
  }
  static_cast<void>(harness.update());
  return std::forward<Predicate>(predicate)();
}

} // namespace

TEST_CASE("public Harness completes an Anthropic SSE turn through Curl") {
  scry::test::LoopbackServer server{response(
      "200 OK", "Content-Type: text/event-stream\r\nrequest-id: req-curl-public\r\n",
      successful_stream)};
  auto harness = scry::Harness::create(config_for(server));
  REQUIRE(harness);
  auto conversation =
      scry::Conversation::create({.system_prompt = "Use the public Curl path."});
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question from app");

  REQUIRE(completion);
  CHECK(completion->text == "Hello from curl.");
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->usage.input_tokens == 7);
  CHECK(completion->usage.output_tokens == 4);
  CHECK(completion->attempt_count == 1);
  CHECK(completion->provider_request_id == "req-curl-public");
  CHECK(conversation->message_count() == 2);

  const auto request = server.request();
  const auto separator = request.find("\r\n\r\n");
  REQUIRE(separator != std::string::npos);
  const auto headers = ascii_lower(request.substr(0, separator));
  const auto body = std::string_view{request}.substr(separator + 4);
  CHECK(headers.starts_with("post /v1/messages http/1.1\r\n"));
  CHECK(headers.find("\r\ncontent-type: application/json\r\n") != std::string::npos);
  CHECK(headers.find("\r\naccept: text/event-stream\r\n") != std::string::npos);
  CHECK(headers.find("\r\nx-api-key: curl-integration-key\r\n") != std::string::npos);
  CHECK(headers.find("\r\nanthropic-version: 2023-06-01\r\n") != std::string::npos);
  CHECK(body.find(R"("model":"test-model")") != std::string_view::npos);
  CHECK(body.find(R"("stream":true)") != std::string_view::npos);
  CHECK(body.find("Question from app") != std::string_view::npos);
  CHECK(body.find("Use the public Curl path.") != std::string_view::npos);
}

TEST_CASE("public Harness completes an OpenAI-compatible SSE turn through Curl") {
  scry::test::LoopbackServer server{response(
      "200 OK", "Content-Type: text/event-stream\r\nx-request-id: req-openai-curl\r\n",
      openai_successful_stream)};
  auto harness = scry::Harness::create(openai_config_for(server));
  REQUIRE(harness);
  auto conversation =
      scry::Conversation::create({.system_prompt = "Use the compatible API."});
  REQUIRE(conversation);

  const auto completion =
      harness->send_and_wait(*conversation, "Question from compatible app");

  REQUIRE(completion);
  CHECK(completion->text == "Hello from OpenAI-compatible curl.");
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->usage.input_tokens == 8);
  CHECK(completion->usage.output_tokens == 5);
  CHECK(completion->provider_request_id == "req-openai-curl");
  CHECK(conversation->message_count() == 2);

  const auto request = server.request();
  const auto separator = request.find("\r\n\r\n");
  REQUIRE(separator != std::string::npos);
  const auto headers = ascii_lower(request.substr(0, separator));
  const auto body = std::string_view{request}.substr(separator + 4);
  CHECK(headers.starts_with("post /v1/chat/completions http/1.1\r\n"));
  CHECK(headers.find("\r\ncontent-type: application/json\r\n") != std::string::npos);
  CHECK(headers.find("\r\naccept: text/event-stream\r\n") != std::string::npos);
  CHECK(headers.find("\r\nauthorization: bearer curl-integration-key\r\n") !=
        std::string::npos);
  CHECK(headers.find("anthropic-") == std::string::npos);
  CHECK(body.find(R"("model":"test-model")") != std::string_view::npos);
  CHECK(body.find(R"("stream":true)") != std::string_view::npos);
  CHECK(body.find(R"("include_usage":true)") != std::string_view::npos);
  CHECK(body.find("Question from compatible app") != std::string_view::npos);
  CHECK(body.find("Use the compatible API.") != std::string_view::npos);
}

TEST_CASE("non-success HTTP status cannot publish an SSE-shaped body") {
  scry::test::LoopbackServer server{
      response("302 Found",
               "Content-Type: text/event-stream\r\nLocation: /redirected\r\n"
               "request-id: req-redirect\r\n",
               successful_stream)};
  auto harness = scry::Harness::create(config_for(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  std::string streamed;
  std::optional<scry::Error> error;
  bool completed = false;
  auto turn = harness->send(
      *conversation, "Do not follow this body",
      {
          .on_text_delta =
              [&streamed](const std::string_view delta) { streamed.append(delta); },
          .on_finished =
              [&completed, &error](scry::Result<scry::Completion> finished) {
                if (finished) {
                  completed = true;
                } else {
                  error = std::move(finished.error());
                }
              },
      });
  REQUIRE(turn);

  REQUIRE(pump_until(*harness, [&] { return error.has_value() || completed; }));
  REQUIRE(error);
  CHECK(error->category == scry::ErrorCategory::protocol);
  CHECK(error->provider_request_id == "req-redirect");
  CHECK(streamed.empty());
  CHECK_FALSE(completed);
  CHECK(conversation->empty());
}

TEST_CASE("HTTP rejection surfaces status and sanitized provider detail through the "
          "public API") {
  constexpr auto anthropic_error_body =
      std::string_view{R"({"type":"error","error":{"type":"not_found_error",)"
                       R"("message":"private-provider-message"}})"};
  scry::test::LoopbackServer server{
      response("404 Not Found",
               "Content-Type: application/json\r\nrequest-id: req-missing-model\r\n",
               anthropic_error_body)};
  auto harness = scry::Harness::create(config_for(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  std::optional<scry::Error> error;
  bool completed = false;
  auto turn = harness->send(
      *conversation, "Ask a model that does not exist",
      {
          .on_finished =
              [&completed, &error](scry::Result<scry::Completion> finished) {
                if (finished) {
                  completed = true;
                } else {
                  error = std::move(finished.error());
                }
              },
      });
  REQUIRE(turn);

  REQUIRE(pump_until(*harness, [&] { return error.has_value() || completed; }));
  REQUIRE(error);
  CHECK(error->category == scry::ErrorCategory::protocol);
  CHECK(error->http_status == 404);
  CHECK(error->provider_detail == "anthropic:not_found_error");
  CHECK(error->provider_request_id == "req-missing-model");
  CHECK(error->message.find("private-provider-message") == std::string::npos);
  CHECK_FALSE(completed);
  CHECK(conversation->empty());
}

TEST_CASE("OpenAI HTTP rejection surfaces its own dialect namespace") {
  constexpr auto openai_error_body =
      std::string_view{R"({"error":{"message":"private-provider-message",)"
                       R"("type":"invalid_request_error","code":"model_not_found"}})"};
  scry::test::LoopbackServer server{
      response("404 Not Found",
               "Content-Type: application/json\r\nx-request-id: req-openai-missing\r\n",
               openai_error_body)};
  auto harness = scry::Harness::create(openai_config_for(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Ask for a bad model");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().http_status == 404);
  CHECK(completion.error().provider_detail == "openai:invalid_request_error");
  CHECK(completion.error().provider_request_id == "req-openai-missing");
  CHECK(completion.error().message.find("private-provider-message") ==
        std::string::npos);
  CHECK(conversation->empty());
}

TEST_CASE("production SSE errors preserve safe provider correlation") {
  constexpr auto error_stream = std::string_view{
      "event: error\n"
      "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
      "\"message\":\"private-provider-message\"},\"request_id\":\"req-body\"}\n\n"};
  scry::test::LoopbackServer server{
      response("200 OK", "Content-Type: text/event-stream\r\n", error_stream)};
  auto harness = scry::Harness::create(config_for(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion =
      harness->send_and_wait(*conversation, "Return a provider error");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().retryable);
  CHECK(completion.error().provider_detail == "anthropic:overloaded_error");
  CHECK(completion.error().provider_request_id == "req-body");
  CHECK(completion.error().message.find("private-provider-message") ==
        std::string::npos);
  CHECK(completion.error().turn_id.has_value());
  CHECK(completion.error().attempt == 1);
  CHECK(conversation->empty());
}

TEST_CASE(
    "active Curl transfer cancellation reaches the public terminal channel promptly") {
  scry::test::LoopbackServer server{
      response("200 OK", "Content-Type: text/event-stream\r\n", successful_stream),
      true};
  auto harness = scry::Harness::create(config_for(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  bool cancelled = false;
  bool completed = false;
  std::optional<scry::Error> error;
  auto turn = harness->send(*conversation, "Cancel this request",
                            {
                                .on_finished =
                                    [&cancelled, &completed,
                                     &error](scry::Result<scry::Completion> finished) {
                                      if (finished) {
                                        completed = true;
                                      } else if (finished.error().category ==
                                                 scry::ErrorCategory::cancelled) {
                                        cancelled = true;
                                      } else {
                                        error = std::move(finished.error());
                                      }
                                    },
                            });
  REQUIRE(turn);
  server.wait_until_request();

  const auto started = std::chrono::steady_clock::now();
  REQUIRE(turn->cancel());
  REQUIRE(pump_until(*harness,
                     [&] { return cancelled || completed || error.has_value(); }));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  CHECK(cancelled);
  CHECK_FALSE(completed);
  CHECK_FALSE(error);
  CHECK(elapsed < 1s);
  CHECK(conversation->empty());
}

TEST_CASE(
    "Harness destruction aborts and joins a held Curl transfer within its bound") {
  scry::test::LoopbackServer server{
      response("200 OK", "Content-Type: text/event-stream\r\n", successful_stream),
      true};
  auto created = scry::Harness::create(config_for(server));
  REQUIRE(created);
  std::optional<scry::Harness> harness{std::move(*created)};
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);
  bool callback_fired = false;
  auto turn = harness->send(
      *conversation, "Destroy this Harness",
      {
          .on_text_delta =
              [&callback_fired](std::string_view) { callback_fired = true; },
          .on_finished = [&callback_fired](
                             scry::Result<scry::Completion>) { callback_fired = true; },
      });
  REQUIRE(turn);
  server.wait_until_request();

  const auto started = std::chrono::steady_clock::now();
  harness.reset();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  CHECK(elapsed < 1s);
  CHECK_FALSE(callback_fired);
  CHECK(conversation->empty());
}
