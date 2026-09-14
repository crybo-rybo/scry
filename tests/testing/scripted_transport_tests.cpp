// The scripted transport is exercised the way a downstream consumer would use
// it: nothing here includes a private Scry header, so a change that breaks a
// consumer's test breaks this suite first.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <scry/scry.hpp>
#include <scry/testing/scripted_transport.hpp>
#include <scry/testing/streams.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

using scry::testing::ScriptedResponse;
using scry::testing::ScriptedTransport;
using scry::testing::ToolUseBlock;

static_assert(std::is_aggregate_v<scry::testing::CapturedRequest>);
static_assert(std::is_aggregate_v<ScriptedResponse>);
static_assert(std::is_default_constructible_v<ScriptedTransport>);
static_assert(std::is_move_constructible_v<ScriptedTransport>);
static_assert(!std::is_copy_constructible_v<ScriptedTransport>);
static_assert(std::is_aggregate_v<ToolUseBlock>);

namespace {

// Port 1 is never listening, so a test that loses its script fails fast instead
// of reaching the network.
[[nodiscard]] scry::Config anthropic_config() {
  auto config = scry::Config{
      .base_url = "http://127.0.0.1:1",
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  return config;
}

[[nodiscard]] scry::Config openai_config() {
  auto config = anthropic_config();
  config.base_url = "http://127.0.0.1:1/v1/";
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

[[nodiscard]] ScriptedResponse answer(std::string body,
                                      std::string request_id = "scripted-request") {
  return {
      .request_id = std::move(request_id),
      .body_chunks = {std::move(body)},
  };
}

template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr std::size_t maximum_pumps = 100'000;
  for (std::size_t pump = 0; pump < maximum_pumps; ++pump) {
    static_cast<void>(harness.update());
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

struct CityArguments {
  std::string city{};
};

// Text and a tool-argument document that only a complete JSON string encoder
// can carry: control characters, quotes, backslashes, and a pretty-printed
// multi-line payload.
constexpr std::string_view tricky_text = "line1\nline2\ttab \"quoted\" back\\slash";
const auto pretty_arguments = std::string{"{\n"
                                          "  \"city\":\n"
                                          "    \"Bos\\tton \\\"MA\\\"\"\n"
                                          "}"};
constexpr std::string_view expected_city = "Bos\tton \"MA\"";

struct RoundTrip {
  std::string text{};
  std::string city{};
};

// Drives a two-round turn through a real Harness and reports what came back
// out of the decoder, so an escaping regression shows up as changed text
// rather than as a decoder error alone.
[[nodiscard]] RoundTrip round_trip(scry::Config config, std::string tool_body,
                                   std::string text_body) {
  ScriptedTransport transport;
  transport.enqueue(answer(std::move(tool_body)));
  transport.enqueue(answer(std::move(text_body)));
  auto harness = scry::testing::create_harness(std::move(config), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  RoundTrip observed;
  REQUIRE(scry::reflection::add<CityArguments>(harness->tools(),
                                               {
                                                   .name = "lookup",
                                                   .description = "Look up a city",
                                               },
                                               [&observed](CityArguments arguments) {
                                                 observed.city =
                                                     std::move(arguments.city);
                                                 return std::string{"sunny"};
                                               }));

  const auto completion = harness->send_and_wait(*conversation, "Weather?");
  REQUIRE(completion);
  observed.text = completion->text;
  return observed;
}

} // namespace

TEST_CASE("scripted Anthropic text turn runs the real decoder and commits") {
  ScriptedTransport transport;
  transport.enqueue(
      answer(scry::testing::anthropic_text_stream("Hello runtime.", "msg_scripted"),
             "anthropic-request"));
  auto harness = scry::testing::create_harness(anthropic_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "Hello runtime.");
  CHECK(completion->finish_reason == scry::FinishReason::completed);
  CHECK(completion->provider_request_id == "anthropic-request");
  CHECK(completion->attempt_count == 1);
  CHECK(conversation->message_count() == 2);
  CHECK(transport.calls() == 1);
  CHECK(transport.remaining() == 0);
}

TEST_CASE("scripted OpenAI-compatible text turn runs the other dialect") {
  ScriptedTransport transport;
  transport.enqueue(answer(scry::testing::openai_text_stream("sunny", "chatcmpl-1"),
                           "openai-request"));
  auto harness = scry::testing::create_harness(openai_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Weather?");
  REQUIRE(completion);
  CHECK(completion->text == "sunny");
  CHECK(completion->usage.input_tokens == 4);
  CHECK(completion->usage.output_tokens == 2);

  const auto requests = transport.requests();
  REQUIRE(requests.size() == 1);
  CHECK(requests.front().url == "http://127.0.0.1:1/v1/chat/completions");
}

TEST_CASE("scripted tool turn dispatches a reflected tool across two rounds") {
  ScriptedTransport transport;
  transport.enqueue(answer(scry::testing::anthropic_tool_stream({
      {.id = "call-a", .name = "lookup", .arguments = R"({"city":"Boston"})"},
  })));
  transport.enqueue(answer(scry::testing::anthropic_text_stream("It is sunny.")));
  auto harness = scry::testing::create_harness(anthropic_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  std::string seen_city;
  REQUIRE(scry::reflection::add<CityArguments>(harness->tools(),
                                               {
                                                   .name = "lookup",
                                                   .description = "Look up a city",
                                               },
                                               [&seen_city](CityArguments arguments) {
                                                 seen_city = arguments.city;
                                                 return std::string{"sunny"};
                                               }));

  std::string observed_tool;
  bool tool_failed = true;
  std::string finished_text;
  auto turn =
      harness->send(*conversation, "Weather in Boston?",
                    {
                        .on_tool_call =
                            [&observed_tool, &tool_failed](const scry::ToolCall& call) {
                              observed_tool = call.name;
                              tool_failed = call.is_error;
                            },
                        .on_finished =
                            [&finished_text](scry::Result<scry::Completion> result) {
                              REQUIRE(result);
                              finished_text = result->text;
                            },
                    });
  REQUIRE(turn);
  REQUIRE(pump_until(*harness, [&finished_text] { return !finished_text.empty(); }));

  CHECK(finished_text == "It is sunny.");
  CHECK(seen_city == "Boston");
  CHECK(observed_tool == "lookup");
  CHECK_FALSE(tool_failed);
  CHECK(conversation->message_count() == 4);

  // The second request must carry the first round's result, which is what makes
  // this an end-to-end check of encoding rather than of the fake.
  const auto requests = transport.requests();
  REQUIRE(requests.size() == 2);
  CHECK(requests.back().body.find("call-a") != std::string::npos);
  CHECK(requests.back().body.find("sunny") != std::string::npos);
}

TEST_CASE("cancelling a held scripted transfer frees the worker without release") {
  ScriptedTransport transport;
  transport.enqueue({
      .request_id = "held-request",
      .body_chunks = {scry::testing::anthropic_text_stream("never delivered")},
      .hold = true,
  });
  transport.enqueue(answer(scry::testing::anthropic_text_stream("second turn")));
  auto harness = scry::testing::create_harness(anthropic_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  bool cancelled = false;
  auto turn =
      harness->send(*conversation, "Question",
                    {
                        .on_finished =
                            [&cancelled](scry::Result<scry::Completion> result) {
                              cancelled = !result && result.error().category ==
                                                         scry::ErrorCategory::cancelled;
                            },
                    });
  REQUIRE(turn);

  transport.wait_for_call(1);
  CHECK(transport.calls() == 1);
  CHECK(turn->cancel());

  // No release(): cancellation alone has to end the held transfer, or the
  // worker never serves another turn.
  REQUIRE(pump_until(*harness, [&cancelled] { return cancelled; }));
  CHECK(conversation->message_count() == 0);

  auto second = scry::Conversation::create();
  REQUIRE(second);
  const auto completion = harness->send_and_wait(*second, "Still there?");
  REQUIRE(completion);
  CHECK(completion->text == "second turn");
  CHECK(transport.calls() == 2);
}

TEST_CASE("releasing a held scripted transfer completes the turn it was holding") {
  ScriptedTransport transport;
  transport.enqueue({
      .request_id = "held-request",
      .body_chunks = {scry::testing::anthropic_text_stream("released")},
      .hold = true,
  });
  auto harness = scry::testing::create_harness(anthropic_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  std::string finished_text;
  auto turn =
      harness->send(*conversation, "Question",
                    {
                        .on_finished =
                            [&finished_text](scry::Result<scry::Completion> result) {
                              REQUIRE(result);
                              finished_text = result->text;
                            },
                    });
  REQUIRE(turn);

  transport.wait_for_call(1);
  CHECK(finished_text.empty());
  transport.release();

  REQUIRE(pump_until(*harness, [&finished_text] { return !finished_text.empty(); }));
  CHECK(finished_text == "released");
}

TEST_CASE("scripted failure is retried and the second answer completes the turn") {
  auto config = anthropic_config();
  config.retry.max_attempts = 2;
  config.retry.initial_backoff = std::chrono::milliseconds{1};
  config.retry.max_backoff = std::chrono::milliseconds{1};
  config.retry.jitter_ratio = 0.0;

  ScriptedTransport transport;
  transport.enqueue({
      .failure =
          scry::Error{
              .category = scry::ErrorCategory::network,
              .retryable = true,
              .message = "scripted transfer failure",
          },
  });
  transport.enqueue(answer(scry::testing::anthropic_text_stream("second try")));
  auto harness = scry::testing::create_harness(config, transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "second try");
  CHECK(completion->attempt_count == 2);
  CHECK(transport.calls() == 2);
}

TEST_CASE("scripted non-2xx status is classified like a real HTTP response") {
  ScriptedTransport transport;
  transport.enqueue({
      .status = 500,
      .body_chunks = {"upstream is unavailable"},
  });
  auto harness = scry::testing::create_harness(openai_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::network);
  CHECK(completion.error().retryable);
  CHECK(completion.error().http_status == 500);
  CHECK(conversation->message_count() == 0);
}

TEST_CASE("a scripted 5xx is retried and the next scripted answer completes the turn") {
  auto config = openai_config();
  config.retry.max_attempts = 2;
  config.retry.initial_backoff = std::chrono::milliseconds{1};
  config.retry.max_backoff = std::chrono::milliseconds{1};

  ScriptedTransport transport;
  transport.enqueue({
      .status = 500,
      .body_chunks = {"upstream is unavailable"},
  });
  transport.enqueue(answer(scry::testing::openai_text_stream("recovered")));
  auto harness = scry::testing::create_harness(config, transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "recovered");
  CHECK(completion->attempt_count == 2);
  CHECK(transport.calls() == 2);
}

TEST_CASE("a scripted OpenAI error body supplies the sanitized provider token") {
  ScriptedTransport transport;
  transport.enqueue({
      .status = 429,
      .body_chunks = {scry::testing::openai_error_body(429, "rate_limit_exceeded")},
  });
  auto harness = scry::testing::create_harness(openai_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::rate_limit);
  CHECK(completion.error().retryable);
  CHECK(completion.error().http_status == 429);
  CHECK(completion.error().provider_detail == "openai:rate_limit_exceeded");
  CHECK(conversation->message_count() == 0);
}

TEST_CASE("a scripted Anthropic error body supplies its own provider token") {
  ScriptedTransport transport;
  transport.enqueue({
      .status = 429,
      .body_chunks = {scry::testing::anthropic_error_body("rate_limit_error")},
  });
  auto harness = scry::testing::create_harness(anthropic_config(), transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::rate_limit);
  CHECK(completion.error().provider_detail == "anthropic:rate_limit_error");
}

TEST_CASE("a scripted 401 is an authentication failure that is not retried") {
  ScriptedTransport transport;
  transport.enqueue({
      .status = 401,
      .body_chunks = {scry::testing::openai_error_body(401, "invalid_api_key")},
  });
  transport.enqueue({
      .status = 401,
      .body_chunks = {scry::testing::anthropic_error_body("authentication_error")},
  });
  auto config = openai_config();
  config.retry.max_attempts = 3;
  auto harness = scry::testing::create_harness(config, transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::authentication);
  CHECK_FALSE(completion.error().retryable);
  CHECK(completion.error().http_status == 401);
  CHECK(completion.error().provider_detail == "openai:invalid_api_key");
  CHECK(transport.calls() == 1);

  auto anthropic = scry::testing::create_harness(anthropic_config(), transport);
  REQUIRE(anthropic);
  auto other = scry::Conversation::create();
  REQUIRE(other);
  const auto second = anthropic->send_and_wait(*other, "Question");
  REQUIRE_FALSE(second);
  CHECK(second.error().category == scry::ErrorCategory::authentication);
  CHECK(second.error().provider_detail == "anthropic:authentication_error");
}

TEST_CASE(
    "scripted Anthropic bodies round-trip text and arguments that need escaping") {
  const auto observed = round_trip(
      anthropic_config(),
      scry::testing::anthropic_tool_stream(
          {
              {.id = R"(call_"a")", .name = "lookup", .arguments = pretty_arguments},
          },
          R"(msg_"tools")"),
      scry::testing::anthropic_text_stream(tricky_text, R"(msg_"text")",
                                           R"(req_"id")"));

  CHECK(observed.text == tricky_text);
  CHECK(observed.city == expected_city);
}

TEST_CASE("scripted OpenAI bodies round-trip text and arguments that need escaping") {
  const auto observed = round_trip(
      openai_config(),
      scry::testing::openai_tool_stream(
          {
              {.id = R"(call_"a")", .name = "lookup", .arguments = pretty_arguments},
          },
          R"(chatcmpl_"tools")"),
      scry::testing::openai_text_stream(tricky_text, R"(chatcmpl_"text")"));

  CHECK(observed.text == tricky_text);
  CHECK(observed.city == expected_city);
}

TEST_CASE("captured requests expose the encoded body and redact nothing else") {
  ScriptedTransport transport;
  auto config = anthropic_config();
  config.extra_headers.push_back({.name = "X-Scry-Test", .value = "captured"});
  transport.enqueue(answer(scry::testing::anthropic_text_stream("ok")));
  auto harness = scry::testing::create_harness(config, transport);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  CHECK(transport.requests().empty());
  REQUIRE(harness->send_and_wait(*conversation, "Inspect me"));

  const auto requests = transport.requests();
  REQUIRE(requests.size() == 1);
  const auto& request = requests.front();
  CHECK(request.url == "http://127.0.0.1:1/v1/messages");
  CHECK(request.body.find("Inspect me") != std::string::npos);
  CHECK(request.body.find("test-model") != std::string::npos);
  const auto extra = std::ranges::find_if(
      request.headers, [](const auto& header) { return header.name == "X-Scry-Test"; });
  REQUIRE(extra != request.headers.end());
  CHECK(extra->value == "captured");
}
