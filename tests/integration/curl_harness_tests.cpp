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
  config.timeouts.transfer = 2s;
  config.timeouts.shutdown = 25ms;
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
  auto turn = harness->send(*conversation, "Do not follow this body");
  REQUIRE(turn);

  std::string streamed;
  std::optional<scry::Error> error;
  bool completed = false;
  REQUIRE(turn->on_text_delta(
      [&streamed](const std::string_view delta) { streamed.append(delta); }));
  REQUIRE(
      turn->on_complete([&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(turn->on_error([&error](const scry::Error& value) { error = value; }));

  REQUIRE(pump_until(*harness, [&] { return error.has_value() || completed; }));
  REQUIRE(error);
  CHECK(error->category == scry::ErrorCategory::protocol);
  CHECK(error->provider_request_id == "req-redirect");
  CHECK(streamed.empty());
  CHECK_FALSE(completed);
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
  auto turn = harness->send(*conversation, "Cancel this request");
  REQUIRE(turn);

  bool cancelled = false;
  bool completed = false;
  std::optional<scry::Error> error;
  REQUIRE(
      turn->on_cancelled([&cancelled](const scry::Cancelled&) { cancelled = true; }));
  REQUIRE(
      turn->on_complete([&completed](const scry::Completion&) { completed = true; }));
  REQUIRE(turn->on_error([&error](const scry::Error& value) { error = value; }));
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
  auto turn = harness->send(*conversation, "Destroy this Harness");
  REQUIRE(turn);

  bool callback_fired = false;
  REQUIRE(turn->on_text_delta(
      [&callback_fired](std::string_view) { callback_fired = true; }));
  REQUIRE(turn->on_complete(
      [&callback_fired](const scry::Completion&) { callback_fired = true; }));
  REQUIRE(
      turn->on_error([&callback_fired](const scry::Error&) { callback_fired = true; }));
  REQUIRE(turn->on_cancelled(
      [&callback_fired](const scry::Cancelled&) { callback_fired = true; }));
  server.wait_until_request();

  const auto started = std::chrono::steady_clock::now();
  harness.reset();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  CHECK(elapsed < 1s);
  CHECK_FALSE(callback_fired);
  CHECK(conversation->empty());
}
