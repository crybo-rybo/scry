#include "support/harness_test_support.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <optional>
#include <scry/scry.hpp>
#include <string>
#include <string_view>
#include <utility>

// The HTTP surface of a turn: the request a provider receives, and how each
// kind of response reaches the public terminal channel.
using namespace std::chrono_literals;
using namespace scry::test_support;

namespace {

const std::string successful_stream =
    anthropic_text_stream("Hello from curl.", "msg_curl", {}, 7, 4);

constexpr auto openai_successful_stream = std::string_view{
    R"(data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}

data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Hello from OpenAI-compatible curl."},"finish_reason":null}]}

data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: {"id":"chatcmpl-curl","object":"chat.completion.chunk","choices":[],"usage":{"prompt_tokens":8,"completion_tokens":5,"total_tokens":13}}

data: [DONE]

)"};

[[nodiscard]] scry::Config curl_config() {
  auto config = test_config();
  config.api_key = "curl-integration-key";
  config.timeouts.connect = 500ms;
  config.timeouts.idle = 2s;
  // No total bound: held transfers are ended by cancellation or destruction.
  config.timeouts.transfer = std::nullopt;
  return config;
}

[[nodiscard]] scry::Config openai_curl_config() {
  auto config = curl_config();
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

[[nodiscard]] bool equal_ignoring_case(const std::string_view left,
                                       const std::string_view right) {
  return std::ranges::equal(left, right, [](const char lhs, const char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
  });
}

// The value of the request header `name`, which HTTP compares without case.
[[nodiscard]] std::optional<std::string>
header(const scry::testing::CapturedRequest& request, const std::string_view name) {
  const auto found = std::ranges::find_if(request.headers, [name](const auto& entry) {
    return equal_ignoring_case(entry.name, name);
  });
  if (found == request.headers.end()) {
    return std::nullopt;
  }
  return found->value;
}

[[nodiscard]] scry::testing::CapturedRequest
only_request(const ScriptedServer& server) {
  const auto requests = server.requests();
  REQUIRE(requests.size() == 1);
  return requests.front();
}

} // namespace

TEST_CASE("public Harness completes an Anthropic SSE turn through Curl") {
  auto fixture = make_harness_fixture(
      curl_config(), {scripted_response(successful_stream, "req-curl-public")});
  auto conversation = unwrap(
      scry::Conversation::create({.system_prompt = "Use the public Curl path."}));

  const auto completion =
      fixture.harness.send_and_wait(conversation, "Question from app");

  REQUIRE(completion);
  CHECK(completion->text == "Hello from curl.");
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->usage.input_tokens == 7);
  CHECK(completion->usage.output_tokens == 4);
  CHECK(completion->attempt_count == 1);
  CHECK(completion->provider_request_id == "req-curl-public");
  CHECK(conversation.message_count() == 2);

  const auto request = only_request(fixture.server);
  CHECK(request.method == "POST");
  CHECK(request.target == "/v1/messages");
  CHECK(header(request, "content-type") == "application/json");
  CHECK(header(request, "accept") == "text/event-stream");
  CHECK(header(request, "x-api-key") == "curl-integration-key");
  CHECK(header(request, "anthropic-version") == "2023-06-01");
  CHECK(request.body.find(R"("model":"test-model")") != std::string::npos);
  CHECK(request.body.find(R"("stream":true)") != std::string::npos);
  CHECK(request.body.find("Question from app") != std::string::npos);
  CHECK(request.body.find("Use the public Curl path.") != std::string::npos);
}

TEST_CASE("public Harness completes an OpenAI-compatible SSE turn through Curl") {
  auto fixture = make_harness_fixture(
      openai_curl_config(),
      {{
          .headers = {{.name = "x-request-id", .value = "req-openai-curl"}},
          .body_chunks = {std::string{openai_successful_stream}},
      }});
  auto conversation =
      unwrap(scry::Conversation::create({.system_prompt = "Use the compatible API."}));

  const auto completion =
      fixture.harness.send_and_wait(conversation, "Question from compatible app");

  REQUIRE(completion);
  CHECK(completion->text == "Hello from OpenAI-compatible curl.");
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->usage.input_tokens == 8);
  CHECK(completion->usage.output_tokens == 5);
  CHECK(completion->provider_request_id == "req-openai-curl");
  CHECK(conversation.message_count() == 2);

  const auto request = only_request(fixture.server);
  CHECK(request.method == "POST");
  CHECK(request.target == "/v1/chat/completions");
  CHECK(header(request, "content-type") == "application/json");
  CHECK(header(request, "accept") == "text/event-stream");
  CHECK(header(request, "authorization") == "Bearer curl-integration-key");
  CHECK(std::ranges::none_of(request.headers, [](const auto& entry) {
    return equal_ignoring_case(entry.name.substr(0, 10), "anthropic-");
  }));
  CHECK(request.body.find(R"("model":"test-model")") != std::string::npos);
  CHECK(request.body.find(R"("stream":true)") != std::string::npos);
  CHECK(request.body.find(R"("include_usage":true)") != std::string::npos);
  CHECK(request.body.find("Question from compatible app") != std::string::npos);
  CHECK(request.body.find("Use the compatible API.") != std::string::npos);
}

TEST_CASE("non-success HTTP status cannot publish an SSE-shaped body") {
  auto fixture = make_harness_fixture(
      curl_config(), {{
                         .status = 302,
                         .headers =
                             {
                                 {.name = "Content-Type", .value = "text/event-stream"},
                                 {.name = "Location", .value = "/redirected"},
                                 {.name = "request-id", .value = "req-redirect"},
                             },
                         .body_chunks = {successful_stream},
                     }});
  std::string streamed;
  std::optional<scry::Error> error;
  bool completed = false;
  auto turn = fixture.harness.send(
      fixture.conversation, "Do not follow this body",
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

  REQUIRE(pump_until(fixture.harness, [&] { return error.has_value() || completed; }));
  REQUIRE(error);
  CHECK(error->category == scry::ErrorCategory::protocol);
  CHECK(error->provider_request_id == "req-redirect");
  CHECK(streamed.empty());
  CHECK_FALSE(completed);
  CHECK(fixture.conversation.empty());
}

TEST_CASE("HTTP rejection surfaces status and sanitized provider detail through the "
          "public API") {
  constexpr auto anthropic_error_body =
      std::string_view{R"({"type":"error","error":{"type":"not_found_error",)"
                       R"("message":"private-provider-message"}})"};
  auto fixture = make_harness_fixture(
      curl_config(),
      {{
          .status = 404,
          .headers = {{.name = "request-id", .value = "req-missing-model"}},
          .body_chunks = {std::string{anthropic_error_body}},
      }});
  std::optional<scry::Error> error;
  bool completed = false;
  auto turn = fixture.harness.send(
      fixture.conversation, "Ask a model that does not exist",
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

  REQUIRE(pump_until(fixture.harness, [&] { return error.has_value() || completed; }));
  REQUIRE(error);
  CHECK(error->category == scry::ErrorCategory::protocol);
  CHECK(error->http_status == 404);
  CHECK(error->provider_detail == "anthropic:not_found_error");
  CHECK(error->provider_request_id == "req-missing-model");
  CHECK(error->message.find("private-provider-message") == std::string::npos);
  CHECK_FALSE(completed);
  CHECK(fixture.conversation.empty());
}

TEST_CASE("OpenAI HTTP rejection surfaces its own dialect namespace") {
  constexpr auto openai_error_body =
      std::string_view{R"({"error":{"message":"private-provider-message",)"
                       R"("type":"invalid_request_error","code":"model_not_found"}})"};
  auto fixture = make_harness_fixture(
      openai_curl_config(),
      {{
          .status = 404,
          .headers = {{.name = "x-request-id", .value = "req-openai-missing"}},
          .body_chunks = {std::string{openai_error_body}},
      }});

  const auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "Ask for a bad model");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().http_status == 404);
  CHECK(completion.error().provider_detail == "openai:invalid_request_error");
  CHECK(completion.error().provider_request_id == "req-openai-missing");
  CHECK(completion.error().message.find("private-provider-message") ==
        std::string::npos);
  CHECK(fixture.conversation.empty());
}

TEST_CASE("production SSE errors preserve safe provider correlation") {
  constexpr auto error_stream = std::string_view{
      "event: error\n"
      "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
      "\"message\":\"private-provider-message\"},\"request_id\":\"req-body\"}\n\n"};
  auto fixture = make_harness_fixture(curl_config(), {scripted_response(error_stream)});

  const auto completion =
      fixture.harness.send_and_wait(fixture.conversation, "Return a provider error");

  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().retryable);
  CHECK(completion.error().provider_detail == "anthropic:overloaded_error");
  CHECK(completion.error().provider_request_id == "req-body");
  CHECK(completion.error().message.find("private-provider-message") ==
        std::string::npos);
  CHECK(completion.error().turn_id.has_value());
  CHECK(completion.error().attempt == 1);
  CHECK(fixture.conversation.empty());
}

TEST_CASE(
    "active Curl transfer cancellation reaches the public terminal channel promptly") {
  auto fixture =
      make_harness_fixture(curl_config(), {{
                                              .body_chunks = {successful_stream},
                                              .hold = true,
                                          }});
  bool cancelled = false;
  bool completed = false;
  std::optional<scry::Error> error;
  auto turn =
      fixture.harness.send(fixture.conversation, "Cancel this request",
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
  REQUIRE(fixture.server.wait_for_request(1));

  const auto started = std::chrono::steady_clock::now();
  REQUIRE(turn->cancel());
  REQUIRE(pump_until(fixture.harness,
                     [&] { return cancelled || completed || error.has_value(); }));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  CHECK(cancelled);
  CHECK_FALSE(completed);
  CHECK_FALSE(error);
  // Promptness is the progress-callback wiring's job; this bound only guards
  // against a hang, so a loaded TSan runner cannot flake it.
  CHECK(elapsed < 5s);
  CHECK(fixture.conversation.empty());
}

TEST_CASE(
    "Harness destruction aborts and joins a held Curl transfer within its bound") {
  auto server = start_server();
  server.enqueue({.body_chunks = {successful_stream}, .hold = true});
  auto config = curl_config();
  config.base_url = server.url();
  std::optional<scry::Harness> harness{unwrap(scry::Harness::create(config))};
  auto conversation = unwrap(scry::Conversation::create());
  bool callback_fired = false;
  auto turn = harness->send(
      conversation, "Destroy this Harness",
      {
          .on_text_delta =
              [&callback_fired](std::string_view) { callback_fired = true; },
          .on_finished = [&callback_fired](
                             scry::Result<scry::Completion>) { callback_fired = true; },
      });
  REQUIRE(turn);
  REQUIRE(server.wait_for_request(1));

  const auto started = std::chrono::steady_clock::now();
  harness.reset();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  // Promptness is the progress-callback wiring's job; this bound only guards
  // against a hang, so a loaded TSan runner cannot flake it.
  CHECK(elapsed < 5s);
  CHECK_FALSE(callback_fired);
  CHECK(conversation.empty());
}
