// The scripted server is exercised the way a downstream consumer would use it:
// nothing here includes a private Scry header, and each Harness comes from the
// public Harness::create, so a change that breaks a consumer's test breaks this
// suite first.
#include <algorithm>
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <netinet/in.h>
#include <optional>
#include <scry/scry.hpp>
#include <scry/testing/scripted_server.hpp>
#include <scry/testing/streams.hpp>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using scry::testing::ScriptedResponse;
using scry::testing::ScriptedServer;
using scry::testing::ToolUseBlock;

static_assert(std::is_aggregate_v<scry::testing::CapturedRequest>);
static_assert(std::is_aggregate_v<ScriptedResponse>);
static_assert(std::is_move_constructible_v<ScriptedServer>);
static_assert(!std::is_copy_constructible_v<ScriptedServer>);
static_assert(std::is_aggregate_v<ToolUseBlock>);

namespace {

[[nodiscard]] ScriptedServer start_server() {
  auto server = ScriptedServer::create();
  REQUIRE(server);
  return std::move(*server);
}

// Millisecond backoffs without jitter keep a scripted retry fast, and a short
// shutdown bound lets a cancelled transfer end promptly.
[[nodiscard]] scry::Config anthropic_config(const ScriptedServer& server) {
  auto config = scry::Config{
      .base_url = server.url(),
      .api_key = "sanitized-test-key",
      .model = "test-model",
  };
  config.retry.max_attempts = 1;
  config.retry.initial_backoff = 1ms;
  config.retry.max_backoff = 5ms;
  config.retry.jitter_ratio = 0.0;
  config.timeouts.shutdown = 25ms;
  return config;
}

[[nodiscard]] scry::Config openai_config(const ScriptedServer& server) {
  auto config = anthropic_config(server);
  config.dialect = scry::ProviderDialect::openai_compatible;
  return config;
}

[[nodiscard]] ScriptedResponse answer(std::string body,
                                      std::string request_id = "scripted-request") {
  return {
      .headers = {{.name = "request-id", .value = std::move(request_id)}},
      .body_chunks = {std::move(body)},
  };
}

// Splits a body in front of `marker`, so the first chunk is everything before it.
[[nodiscard]] std::vector<std::string> split_before(const std::string& body,
                                                    const std::string_view marker) {
  const auto at = body.find(marker);
  REQUIRE(at != std::string::npos);
  return {body.substr(0, at), body.substr(at)};
}

template <typename Predicate>
[[nodiscard]] bool pump_until(scry::Harness& harness, Predicate&& predicate) {
  constexpr auto deadline = 10s;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < deadline) {
    static_cast<void>(harness.update());
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
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
[[nodiscard]] RoundTrip round_trip(const scry::ProviderDialect dialect,
                                   std::string tool_body, std::string text_body) {
  auto server = start_server();
  server.enqueue(answer(std::move(tool_body)));
  server.enqueue(answer(std::move(text_body)));
  auto config = anthropic_config(server);
  config.dialect = dialect;
  auto harness = scry::Harness::create(std::move(config));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  RoundTrip observed;
  REQUIRE(harness->tools().add<CityArguments>(
      {
          .name = "lookup",
          .description = "Look up a city",
      },
      [&observed](CityArguments arguments) {
        observed.city = std::move(arguments.city);
        return std::string{"sunny"};
      }));

  const auto completion = harness->send_and_wait(*conversation, "Weather?");
  REQUIRE(completion);
  observed.text = completion->text;
  return observed;
}

} // namespace

TEST_CASE("scripted Anthropic text turn runs the real decoder and commits") {
  auto server = start_server();
  server.enqueue(
      answer(scry::testing::anthropic_text_stream("Hello runtime.", "msg_scripted"),
             "anthropic-request"));
  auto harness = scry::Harness::create(anthropic_config(server));
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
  CHECK(server.requests().size() == 1);
  CHECK(server.remaining() == 0);
}

TEST_CASE("scripted Anthropic turn skips thinking and commits only the text") {
  // An Anthropic-compatible server, such as Ollama, can send thinking without a
  // request for it.
  constexpr std::string_view body = R"(event: message_start
data: {"type":"message_start","message":{"id":"msg_thinking","type":"message","role":"assistant","content":[],"model":"test-model","stop_reason":null,"usage":{"input_tokens":2,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Hidden."}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: content_block_start
data: {"type":"content_block_start","index":1,"content_block":{"type":"redacted_thinking","data":"x"}}

event: content_block_stop
data: {"type":"content_block_stop","index":1}

event: content_block_start
data: {"type":"content_block_start","index":2,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":2,"delta":{"type":"text_delta","text":"Visible."}}

event: content_block_stop
data: {"type":"content_block_stop","index":2}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}}

event: message_stop
data: {"type":"message_stop"}

)";

  auto server = start_server();
  server.enqueue(answer(std::string{body}));
  auto harness = scry::Harness::create(anthropic_config(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "Visible.");
  const auto& messages = conversation->messages();
  REQUIRE(messages.size() == 2);
  const auto& reply = messages.back();
  CHECK(reply.role == scry::Role::assistant);
  REQUIRE(reply.content.size() == 1);
  const auto* text = std::get_if<scry::TextBlock>(&reply.content.front());
  REQUIRE(text != nullptr);
  CHECK(text->text == "Visible.");
}

TEST_CASE("scripted OpenAI-compatible text turn runs the other dialect") {
  auto server = start_server();
  server.enqueue(answer(scry::testing::openai_text_stream("sunny", "chatcmpl-1"),
                        "openai-request"));
  auto harness = scry::Harness::create(openai_config(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Weather?");
  REQUIRE(completion);
  CHECK(completion->text == "sunny");
  CHECK(completion->usage.input_tokens == 4);
  CHECK(completion->usage.output_tokens == 2);

  const auto requests = server.requests();
  REQUIRE(requests.size() == 1);
  CHECK(requests.front().method == "POST");
  CHECK(requests.front().target == "/v1/chat/completions");
}

TEST_CASE("scripted tool turn dispatches a reflected tool across two rounds") {
  auto server = start_server();
  server.enqueue(answer(scry::testing::anthropic_tool_stream({
      {.id = "call-a", .name = "lookup", .arguments = R"({"city":"Boston"})"},
  })));
  server.enqueue(answer(scry::testing::anthropic_text_stream("It is sunny.")));
  auto harness = scry::Harness::create(anthropic_config(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  std::string seen_city;
  REQUIRE(harness->tools().add<CityArguments>(
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
  // this an end-to-end check of encoding rather than of the script.
  const auto requests = server.requests();
  REQUIRE(requests.size() == 2);
  CHECK(requests.back().body.find("call-a") != std::string::npos);
  CHECK(requests.back().body.find("sunny") != std::string::npos);
}

TEST_CASE(
    "cancelling a held response frees the worker and the server without release") {
  auto server = start_server();
  server.enqueue({
      .body_chunks = {scry::testing::anthropic_text_stream("never delivered")},
      .hold = true,
  });
  server.enqueue(answer(scry::testing::anthropic_text_stream("second turn")));
  auto harness = scry::Harness::create(anthropic_config(server));
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

  REQUIRE(server.wait_for_request(1));
  CHECK(turn->cancel());

  // No release(): cancellation alone has to end the held transfer, and the
  // server has to notice the closed connection, or no other turn is served.
  REQUIRE(pump_until(*harness, [&cancelled] { return cancelled; }));
  CHECK(conversation->message_count() == 0);

  auto second = scry::Conversation::create();
  REQUIRE(second);
  const auto completion = harness->send_and_wait(*second, "Still there?");
  REQUIRE(completion);
  CHECK(completion->text == "second turn");
  CHECK(server.requests().size() == 2);
}

TEST_CASE("releasing a held response completes the turn it was holding") {
  auto server = start_server();
  server.enqueue({
      .body_chunks = {scry::testing::anthropic_text_stream("released")},
      .hold = true,
  });
  auto harness = scry::Harness::create(anthropic_config(server));
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

  REQUIRE(server.wait_for_request(1));
  CHECK(finished_text.empty());
  server.release();

  REQUIRE(pump_until(*harness, [&finished_text] { return !finished_text.empty(); }));
  CHECK(finished_text == "released");
}

TEST_CASE("a connection closed before any output is retried and the next answer "
          "completes the turn") {
  auto server = start_server();
  server.enqueue({.close_after_chunks = 0});
  server.enqueue(answer(scry::testing::anthropic_text_stream("second try")));
  auto config = anthropic_config(server);
  config.retry.max_attempts = 2;
  auto harness = scry::Harness::create(config);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "second try");
  CHECK(completion->attempt_count == 2);
  CHECK(server.requests().size() == 2);
}

TEST_CASE("a connection closed after partial output fails the turn without a retry") {
  auto server = start_server();
  server.enqueue({
      .body_chunks = split_before(scry::testing::anthropic_text_stream("partial"),
                                  "event: message_delta"),
      .close_after_chunks = 1,
  });
  server.enqueue(answer(scry::testing::anthropic_text_stream("never requested")));
  auto config = anthropic_config(server);
  config.retry.max_attempts = 3;
  auto harness = scry::Harness::create(config);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  std::string streamed;
  std::optional<scry::Result<scry::Completion>> finished;
  auto turn = harness->send(
      *conversation, "Question",
      {
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed += text; },
          .on_finished =
              [&finished](scry::Result<scry::Completion> result) {
                finished = std::move(result);
              },
      });
  REQUIRE(turn);
  REQUIRE(pump_until(*harness, [&finished] { return finished.has_value(); }));

  // The transfer failed as a retryable network error, but text was consumed,
  // so the turn ends on the first attempt and commits nothing.
  CHECK(streamed == "partial");
  REQUIRE_FALSE(*finished);
  const auto& error = finished->error();
  CHECK(error.category == scry::ErrorCategory::network);
  CHECK(error.retryable);
  CHECK(error.attempt == 1);
  CHECK(server.requests().size() == 1);
  CHECK(server.remaining() == 1);
  CHECK(conversation->message_count() == 0);
}

TEST_CASE("a pause after a chunk lets the host cancel after a text delta arrived") {
  auto server = start_server();
  server.enqueue({
      .body_chunks = split_before(scry::testing::anthropic_text_stream("first words"),
                                  "event: message_delta"),
      .pause_after_chunks = 1,
  });
  server.enqueue(answer(scry::testing::anthropic_text_stream("next turn")));
  auto harness = scry::Harness::create(anthropic_config(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  std::string streamed;
  std::optional<scry::Result<scry::Completion>> finished;
  auto turn = harness->send(
      *conversation, "Question",
      {
          .on_text_delta =
              [&streamed](const std::string_view text) { streamed += text; },
          .on_finished =
              [&finished](scry::Result<scry::Completion> result) {
                finished = std::move(result);
              },
      });
  REQUIRE(turn);
  REQUIRE(pump_until(*harness, [&streamed] { return !streamed.empty(); }));
  CHECK(streamed == "first words");
  CHECK_FALSE(finished);

  CHECK(turn->cancel());
  REQUIRE(pump_until(*harness, [&finished] { return finished.has_value(); }));
  REQUIRE_FALSE(*finished);
  CHECK(finished->error().category == scry::ErrorCategory::cancelled);
  CHECK(conversation->message_count() == 0);

  // The paused response ended with the closed connection, so the server is
  // free for the next turn.
  const auto completion = harness->send_and_wait(*conversation, "Again");
  REQUIRE(completion);
  CHECK(completion->text == "next turn");
}

TEST_CASE("max_backoff bounds the delay that a Retry-After header asks for") {
  auto server = start_server();
  server.enqueue({
      .status = 429,
      .headers = {{.name = "Retry-After", .value = "1"}},
      .body_chunks = {scry::testing::openai_error_body(429, "rate_limit_exceeded")},
  });
  server.enqueue(answer(scry::testing::openai_text_stream("after the limit")));
  auto config = openai_config(server);
  config.retry.max_attempts = 2;
  auto harness = scry::Harness::create(config);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto start = std::chrono::steady_clock::now();
  const auto completion = harness->send_and_wait(*conversation, "Question");
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(completion);
  CHECK(completion->text == "after the limit");
  CHECK(completion->attempt_count == 2);
  CHECK(server.requests().size() == 2);
  // An honoured Retry-After of one second would take at least one second.
  CHECK(elapsed < 1s);
}

TEST_CASE("a request with no scripted response left fails the turn visibly") {
  auto server = start_server();
  CHECK_FALSE(server.wait_for_request(1, 1ms));
  auto config = anthropic_config(server);
  config.retry.max_attempts = 3;
  auto harness = scry::Harness::create(config);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::protocol);
  CHECK(completion.error().http_status == 404);
  CHECK(completion.error().provider_detail == "anthropic:unscripted_request");
  CHECK(server.requests().size() == 1);
}

TEST_CASE("scripted non-2xx status is classified like a real HTTP response") {
  auto server = start_server();
  server.enqueue({
      .status = 500,
      .body_chunks = {"upstream is unavailable"},
  });
  auto harness = scry::Harness::create(openai_config(server));
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
  auto server = start_server();
  server.enqueue({
      .status = 500,
      .body_chunks = {"upstream is unavailable"},
  });
  server.enqueue(answer(scry::testing::openai_text_stream("recovered")));
  auto config = openai_config(server);
  config.retry.max_attempts = 2;
  auto harness = scry::Harness::create(config);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE(completion);
  CHECK(completion->text == "recovered");
  CHECK(completion->attempt_count == 2);
  CHECK(server.requests().size() == 2);
}

TEST_CASE("a scripted OpenAI error body supplies the sanitized provider token") {
  auto server = start_server();
  server.enqueue({
      .status = 429,
      .body_chunks = {scry::testing::openai_error_body(429, "rate_limit_exceeded")},
  });
  auto harness = scry::Harness::create(openai_config(server));
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
  auto server = start_server();
  server.enqueue({
      .status = 429,
      .body_chunks = {scry::testing::anthropic_error_body("rate_limit_error")},
  });
  auto harness = scry::Harness::create(anthropic_config(server));
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::rate_limit);
  CHECK(completion.error().provider_detail == "anthropic:rate_limit_error");
}

TEST_CASE("a scripted 401 is an authentication failure that is not retried") {
  auto server = start_server();
  server.enqueue({
      .status = 401,
      .body_chunks = {scry::testing::openai_error_body(401, "invalid_api_key")},
  });
  server.enqueue({
      .status = 401,
      .body_chunks = {scry::testing::anthropic_error_body("authentication_error")},
  });
  auto config = openai_config(server);
  config.retry.max_attempts = 3;
  auto harness = scry::Harness::create(config);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  const auto completion = harness->send_and_wait(*conversation, "Question");
  REQUIRE_FALSE(completion);
  CHECK(completion.error().category == scry::ErrorCategory::authentication);
  CHECK_FALSE(completion.error().retryable);
  CHECK(completion.error().http_status == 401);
  CHECK(completion.error().provider_detail == "openai:invalid_api_key");
  CHECK(server.requests().size() == 1);

  auto anthropic = scry::Harness::create(anthropic_config(server));
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
      scry::ProviderDialect::anthropic,
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
      scry::ProviderDialect::openai_compatible,
      scry::testing::openai_tool_stream(
          {
              {.id = R"(call_"a")", .name = "lookup", .arguments = pretty_arguments},
          },
          R"(chatcmpl_"tools")"),
      scry::testing::openai_text_stream(tricky_text, R"(chatcmpl_"text")"));

  CHECK(observed.text == tricky_text);
  CHECK(observed.city == expected_city);
}

TEST_CASE("captured requests expose the method, target, headers, and body") {
  auto server = start_server();
  auto config = anthropic_config(server);
  config.extra_headers.push_back({.name = "X-Scry-Test", .value = "captured"});
  server.enqueue(answer(scry::testing::anthropic_text_stream("ok")));
  auto harness = scry::Harness::create(config);
  REQUIRE(harness);
  auto conversation = scry::Conversation::create();
  REQUIRE(conversation);

  CHECK(server.requests().empty());
  REQUIRE(harness->send_and_wait(*conversation, "Inspect me"));

  const auto requests = server.requests();
  REQUIRE(requests.size() == 1);
  const auto& request = requests.front();
  CHECK(request.method == "POST");
  CHECK(request.target == "/v1/messages");
  CHECK(request.body.find("Inspect me") != std::string::npos);
  CHECK(request.body.find("test-model") != std::string::npos);
  const auto extra = std::ranges::find_if(
      request.headers, [](const auto& header) { return header.name == "X-Scry-Test"; });
  REQUIRE(extra != request.headers.end());
  CHECK(extra->value == "captured");
}

namespace {

// Connects a raw client with a tiny receive buffer and sends one request, then
// never reads, so a large response fills the server's send buffer.
[[nodiscard]] int connect_stalled_reader(const ScriptedServer& server) {
  const auto url = server.url();
  const auto port_text = std::string_view{url}.substr(url.rfind(':') + 1);
  std::uint16_t port = 0;
  REQUIRE(
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), port).ec ==
      std::errc{});

  const auto client = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(client >= 0);
  constexpr int receive_buffer = 1024;
  REQUIRE(::setsockopt(client, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                       sizeof(receive_buffer)) == 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(client, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) == 0);
  constexpr std::string_view request =
      "POST /v1/messages HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n";
  REQUIRE(::send(client, request.data(), request.size(), 0) ==
          static_cast<ssize_t>(request.size()));
  return client;
}

} // namespace

TEST_CASE("destroying the server does not wait on a client that stopped reading") {
  auto server = start_server();
  // Far more than a loopback send buffer holds, so the response cannot finish
  // while the client is not reading.
  constexpr std::size_t body_size = std::size_t{32} * 1024 * 1024;
  server.enqueue({.body_chunks = {std::string(body_size, 'x')}});
  const auto client = connect_stalled_reader(server);
  REQUIRE(server.wait_for_request(1));

  std::promise<void> destroyed;
  auto done = destroyed.get_future();
  std::thread destroyer{
      [stopping = std::move(server), destroyed = std::move(destroyed)]() mutable {
        {
          const auto last = std::move(stopping);
        }
        destroyed.set_value();
      }};
  const auto finished = done.wait_for(5s) == std::future_status::ready;
  // Closing the client fails a stuck write, so the destroyer can be joined
  // either way.
  ::close(client);
  destroyer.join();
  CHECK(finished);
}
